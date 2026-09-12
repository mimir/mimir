#include "mim/driver.h"

#include <algorithm>
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

/// Install tree @p libmim_path belongs to, i.e. the directory whose `<MIM_LIBDIR>/mim` holds the plugins.
std::optional<fs::path> prefix_of(const fs::path& libmim_path) {
    for (auto dir = libmim_path.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        std::error_code ignore;
        if (fs::is_directory(dir / MIM_LIBDIR / "mim", ignore) && !ignore) return dir;
        if (dir == dir.root_path()) break;
    }

    return {};
}

std::optional<fs::path> path_to_libmim() { return fe::sys::path_to_lib((const void*)&mim_lib_anchor); }

/// A prefix may derive what a plain directory already names, and probing it twice only slows lookup down.
void push(fe::Vector<fs::path>& paths, fs::path path) {
    if (std::ranges::find(paths, path) == paths.end()) paths.emplace_back(std::move(path));
}

} // namespace

void Driver::Imports::add(const fe::Src* src, Sym sym, ast::Tok::Tag tag, bool is_path) {
    // The SrcMap interns paths, so one file is one fe::Src - comparing those settles "same file".
    // Aliases (`as`) must not add a second entry, so the spelling is not part of the key.
    for (const auto& entry : entries_)
        if (entry.tag == tag && entry.src == src) return;

    entries_.emplace_back(Entry{src, sym, tag, is_path});
}

Driver::Driver(std::string name)
    : fe::Driver(std::make_unique<Diag>(*this))
    , version_(MIM_VERSION)
    , world_(this, sym(name)) {
    auto from_env = [](const char* var, auto&& add) {
        if (auto env = std::getenv(var)) {
            auto stream = std::stringstream{env};
            auto path   = std::string{};
            while (std::getline(stream, path, fe::sys::Path_Sep))
                add(fs::path{path});
        }
    };

    from_env("MIM_PLUGIN_PATH", [this](fs::path path) { add_plugin_path(std::move(path)); });
    from_env("MIM_IMPORT_PATH", [this](fs::path path) { add_import_path(std::move(path)); });
    from_env("MIM_PREFIX_PATH", [this](fs::path path) { add_prefix_path(std::move(path)); });

    if (auto path = path_to_libmim()) {
        // A layout that keeps the plugins right next to libmim has no prefix to derive them from.
        add_plugin_path(path->parent_path() / "mim");
        if (auto prefix = prefix_of(*path)) add_prefix_path(*std::move(prefix));
    }

    add_prefix_path(fs::path{MIM_INSTALL_PREFIX});

    // User paths are added later and must be searched before the environment and derived ones.
    plugin_dirs_.seal();
    import_dirs_.seal();
    prefixes_.seal();
}

fe::Vector<fs::path> Driver::plugin_paths() const {
    auto res = fe::Vector<fs::path>();
    res.emplace_back();
    for (const auto& dir : plugin_dirs_)
        push(res, dir);
    for (const auto& prefix : prefixes_)
        push(res, prefix / MIM_LIBDIR / "mim");
    return res;
}

fe::Vector<fs::path> Driver::import_paths() const {
    auto res = fe::Vector<fs::path>();
    res.emplace_back();
    for (const auto& dir : import_dirs_)
        push(res, dir);
    for (const auto& dir : plugin_dirs_)
        push(res, dir);
    for (const auto& prefix : prefixes_) {
        push(res, prefix / MIM_DATADIR / "mim");
        push(res, prefix / MIM_LIBDIR / "mim");
    }
    return res;
}

fe::Vector<fs::path> Driver::rt_paths() const {
    auto res = fe::Vector<fs::path>();
    res.emplace_back("rt");
    for (const auto& dir : plugin_dirs_)
        push(res, dir / "rt");
    for (const auto& prefix : prefixes_)
        push(res, prefix / MIM_LIBDIR / "mim" / "rt");
    return res;
}

void Driver::load(std::string_view name) {
    log().i("💾 load plugin `{}`", name);

    if (is_loaded(name)) {
        log().w("plugin `{}` already loaded", name);
        return;
    }

    auto handle = Plugin::Handle{nullptr, fe::dl::close};
    auto dir    = fs::path{};
    if (auto path = fs::path{name}; path.is_absolute() && fs::is_regular_file(path)) {
        auto path_str = path.string();
        if (handle.reset(fe::dl::open(path_str.c_str())); handle) dir = path.parent_path();
    }
    if (!handle) {
        for (const auto& path : plugin_paths()) {
            auto full_path = path / std::format("libmim_{}.{}", name, fe::dl::Ext);
            std::error_code ignore;
            if (bool reg_file = fs::is_regular_file(full_path, ignore); reg_file && !ignore) {
                auto path_str = full_path.string();
                if (handle.reset(fe::dl::open(path_str.c_str())); handle) {
                    dir = path;
                    break;
                }
            }
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
        fe::assert_emplace(plugins_, std::string(name), std::move(handle));
        plugin2dir_.emplace(std::string(name), std::move(dir));
        // clang-format off
        if (auto reg = plugin.register_normalizers) reg(normalizers_);
        if (auto reg = plugin.register_phases)      reg(phases_);
        // clang-format on
        if (plugin.args) known_args_.emplace_back(name, fe::View<PluginArg>(plugin.args, plugin.num_args));
        if (plugin.envs) known_envs_.emplace_back(name, fe::View<PluginEnv>(plugin.envs, plugin.num_envs));
    } else {
        fe::throwf("plugin `{}` has no `mim_get_plugin()`", name);
    }
}

void* Driver::get_fun_ptr(std::string_view plugin, const char* name) {
    if (auto handle = fe::lookup(plugins_, plugin)) return fe::dl::get(handle->get(), name);
    return nullptr;
}

const fe::Vector<std::string>& Driver::args(std::string_view plugin) const {
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

bool PlainNames::active(const Driver& driver) { return driver.names().depth != 0; }

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
