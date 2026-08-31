#include "mim/driver.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

#include <fe/dl.h>
#include <fe/sys.h>

#include "mim/config.h"
#include "mim/plugin.h"

// Any address inside libmim identifies the shared object it was loaded from; see path_to_libmim.
extern "C" MIM_EXPORT void mim_lib_anchor() {}

namespace mim {

namespace {

bool has_plugin_dir(const fs::path& libmim_path) {
    std::error_code ignore;
    return fs::is_directory(libmim_path.parent_path() / "mim", ignore) && !ignore;
}

fs::path adjust_libmim_path(const fs::path& libmim_path) {
    if (has_plugin_dir(libmim_path)) return libmim_path;

    auto dir      = libmim_path.parent_path();
    auto lib_name = libmim_path.filename();
    while (!dir.empty()) {
        if (dir == dir.root_path()) break;

        std::error_code ignore;
        auto candidate = dir / MIM_LIBDIR / "mim";
        if (fs::is_directory(candidate, ignore) && !ignore) return candidate.parent_path() / lib_name;

        dir = dir.parent_path();
    }

    return libmim_path;
}

/// Path of libmim itself, adjusted so that `<parent>/mim` resolves to the default in-tree plugin directory.
std::optional<fs::path> path_to_libmim() {
    if (auto path = fe::sys::path_to_lib((const void*)&mim_lib_anchor)) return adjust_libmim_path(*path);
    return {};
}

} // namespace

std::pair<const fe::Src*, bool> Driver::Imports::add(fs::path path, Sym sym, ast::Tok::Tag tag) {
    auto [src, fresh] = driver_.src().add(std::move(path));
    if (!src) return {nullptr, false};

    // The SrcMap interns paths, so one file is one fe::Src - comparing those settles "same file".
    bool seen_entry = false;
    for (const auto& entry : entries_) {
        if (entry.sym == sym && entry.tag == tag && entry.src == src) {
            seen_entry = true;
            break;
        }
    }

    if (!seen_entry) entries_.emplace_back(Entry{src, sym, tag});
    return {src, fresh};
}

Driver::Driver(std::string name)
    : version_(MIM_VERSION)
    , world_(this, sym(name))
    , imports_(*this) {
    diag(std::make_unique<Diag>(*this));

    // prepend empty path
    search_paths_.emplace_front(fs::path{});

    // paths from env
    if (auto env_path = std::getenv("MIM_PLUGIN_PATH")) {
        std::stringstream env_path_stream{env_path};
        std::string sub_path;
        while (std::getline(env_path_stream, sub_path, ':'))
            add_search_path(sub_path);
    }

    // add <path/to/libmim>/mim
    if (auto path = path_to_libmim()) add_search_path(path->parent_path() / "mim");

    // add install path if different from above
    if (auto install_path = fs::path{MIM_INSTALL_PREFIX} / MIM_LIBDIR / "mim"; fs::exists(install_path)) {
        if (search_paths().size() < 2 || !fs::equivalent(install_path, search_paths().back()))
            add_search_path(std::move(install_path));
    }

    // all other user paths are placed just behind the first path (the empty path)
    insert_ = ++search_paths_.begin();
}

void Driver::load(Sym name) {
    log().i("💾 load plugin `{}`", name);

    if (is_loaded(name)) {
        log().w("plugin `{}` already loaded", name);
        return;
    }

    auto handle = Plugin::Handle{nullptr, fe::dl::close};
    if (auto path = fs::path{name.view()}; path.is_absolute() && fs::is_regular_file(path))
        handle.reset(fe::dl::open(name.c_str()));
    if (!handle) {
        for (const auto& path : search_paths()) {
            auto full_path = path / std::format("libmim_{}.{}", name, fe::dl::extension);
            std::error_code ignore;
            if (bool reg_file = fs::is_regular_file(full_path, ignore); reg_file && !ignore) {
                auto path_str = full_path.string();
                if (handle.reset(fe::dl::open(path_str.c_str())); handle) break;
            }
            if (handle) break;
        }
    }

    if (!handle) fe::throwf("cannot open plugin `{}`", name);

    if (auto get_info = reinterpret_cast<decltype(&mim_get_plugin)>(fe::dl::get(handle.get(), "mim_get_plugin"))) {
        auto plugin = get_info();
        if (version() != plugin.version) {
            std::ostringstream oss;
            std::print(oss, "plugin {} has version {} while MimIR has version {}", plugin.name, plugin.version,
                       version());
            if (flags().force_load)
                std::cerr << "warning: " << oss.str() << '\n';
            else
                throw std::logic_error(oss.str());
        }
        fe::assert_emplace(plugins_, name, std::move(handle));
        // clang-format off
        if (auto reg = plugin.register_normalizers) reg(normalizers_);
        if (auto reg = plugin.register_phases)      reg(phases_);
        // clang-format on
    } else {
        fe::throwf("plugin `{}` has no `mim_get_plugin()`", name);
    }
}

void* Driver::get_fun_ptr(Sym plugin, const char* name) {
    if (auto handle = fe::lookup(plugins_, plugin)) return fe::dl::get(handle->get(), name);
    return nullptr;
}

const fe::Vector<std::string>& Driver::args(Sym plugin) const {
    static const fe::Vector<std::string> empty;
    if (auto i = plugin_args_.find(plugin); i != plugin_args_.end()) return i->second;
    return empty;
}

PlainNames::PlainNames(const Driver* driver)
    : driver_(driver) {
    if (!driver_) return;

    auto& names = driver_->names();
    if (names.depth++ == 0) {
        names.clashed = false;
        names.sym2gid.clear();
    }
}

PlainNames::~PlainNames() {
    if (driver_) --driver_->names().depth;
}

bool PlainNames::clashed() const { return driver_ && driver_->names().clashed; }

bool PlainNames::claim(const Driver& driver, Sym sym, u32 gid) {
    auto& names = driver.names();
    if (names.depth == 0) return false;
    if (auto [i, ins] = names.sym2gid.emplace(sym, gid); !ins && i->second != gid) names.clashed = true;
    return true;
}

std::string Diag::render(const std::function<std::string()>& fmt) const {
    bool clashed = false;
    auto str     = std::string();
    {
        auto plain = PlainNames(&driver_);
        str        = CodeDiag::render(fmt);
        clashed    = plain.clashed();
    }
    return clashed ? CodeDiag::render(fmt) : str; // the retry must run outside the guard, or it renders plainly again
}

} // namespace mim
