#include "mim/driver.h"

#include <cstring>

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

/// Function-local so that Driver::add_static_plugin works during static initialization.
absl::flat_hash_map<std::string, Plugin (*)()>& static_plugins() {
    static auto map = absl::flat_hash_map<std::string, Plugin (*)()>();
    return map;
}

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
#define CODE(t, str) keys_.emplace(sym(str), ast::Tok::Tag::t);
    MIM_KEY(CODE)
#undef CODE

#define CODE(str, t) \
    if (ast::Tok::Tag::t != ast::Tok::Tag::Nil) keys_.emplace(sym(str), ast::Tok::Tag::t);
    MIM_SUBST(CODE)
#undef CODE

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

std::string Driver::plugin_name(std::string_view name) {
    constexpr auto prefix = std::string_view("libmim_");
    auto stem             = fs::path(name).stem().string();
    if (stem.starts_with(prefix)) stem.erase(0, prefix.size());
    return stem;
}

void Driver::add_static_plugin(const char* name, Plugin (*get_plugin)()) { static_plugins()[name] = get_plugin; }

void Driver::load(std::string_view spec) {
    auto name = plugin_name(spec);
    log().i("💾 load plugin `{}`", spec);

    if (is_loaded(name)) {
        log().w("plugin `{}` already loaded", name);
        return;
    }

    auto handle   = Plugin::Handle{nullptr, fe::dl::close};
    auto dir      = fs::path{};
    auto path     = fs::path{spec};
    auto sub      = path.parent_path();
    auto get_info = decltype(&mim_get_plugin){};

    // `foo/bar` looks below the `foo` of each search path, so that both halves stay together.
    auto find = [&, this](const std::string& file, auto&& accept) {
        for (const auto& search : plugin_paths())
            if (std::error_code ec; fs::is_regular_file(search / sub / file, ec) && accept(search / sub / file))
                return search / sub;
        return fs::path{};
    };

    if (auto get = fe::lookup(static_plugins(), name)) {
        get_info = *get;
        // No shared object pins the directory, so the `<name>.mim` half is searched for just like an import.
        dir = find(std::format("{}.mim", name), [](const fs::path&) { return true; });
    } else {
        auto ext  = std::format(".{}", fe::dl::Ext);
        auto open = [&handle](const fs::path& p) {
            auto str = p.string();
            return handle.reset(fe::dl::open(str.c_str())), bool(handle);
        };

        // Only a spec naming the shared object itself is opened as-is; `foo/bar.mim` still wants `foo/libmim_bar`.
        if (path.is_absolute() && path.extension() == ext && fs::is_regular_file(path) && open(path))
            dir = path.parent_path();
        if (!handle) dir = find(std::format("libmim_{}{}", name, ext), open);

        if (!handle) fe::throwf("cannot open plugin `{}`", spec);
        get_info = reinterpret_cast<decltype(&mim_get_plugin)>(fe::dl::get(handle.get(), "mim_get_plugin"));
        if (!get_info) fe::throwf("plugin `{}` has no `mim_get_plugin()`", name);
    }

    auto plugin = get_info();
    if (version() != plugin.version) {
        std::ostringstream oss;
        std::print(oss, "plugin {} has version {} while MimIR has version {}", plugin.name, plugin.version, version());
        if (flags().force_load)
            std::cerr << "warning: " << oss.str() << '\n';
        else
            throw std::logic_error(oss.str());
    }
    fe::assert_emplace(plugins_, name,
                       Loaded{std::move(handle), std::move(dir), fe::View<PluginSym>(plugin.syms, plugin.num_syms)});
    // clang-format off
    if (auto reg = plugin.register_normalizers) reg(normalizers_);
    if (auto reg = plugin.register_phases)      reg(phases_);
    // clang-format on
    if (plugin.args) known_args_.emplace_back(name, fe::View<PluginArg>(plugin.args, plugin.num_args));
    if (plugin.envs) known_envs_.emplace_back(name, fe::View<PluginEnv>(plugin.envs, plugin.num_envs));
}

void* Driver::get_fun_ptr(std::string_view plugin, const char* name) {
    auto loaded = fe::lookup(plugins_, plugin);
    if (!loaded) return nullptr;
    for (const auto& sym : loaded->syms)
        if (std::strcmp(sym.name, name) == 0) return sym.ptr;
    return loaded->handle ? fe::dl::get(loaded->handle.get(), name) : nullptr;
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
