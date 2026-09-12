#pragma once

#include <filesystem>
#include <list>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>
#include <fe/driver.h>
#include <fe/log.h>
#include <fe/profile.h>

#include "mim/flags.h"
#include "mim/plugin.h"
#include "mim/world.h"

#include "mim/ast/tok.h"

namespace mim {

namespace fs = std::filesystem;

class Driver;

/// Renders Def%s with their plain Def::sym instead of Def::unique_name while alive.
/// A gid is noise in a diagnostic about the user's source - but it is also the only thing that tells two
/// same-named Def%s apart, so PlainNames::clashed reports when a message has to be rendered again with gids.
/// The state lives in Driver::names, so two Driver%s formatting at once never share it.
class PlainNames {
public:
    /// Activates plain naming on @p driver until this guard dies; a null @p driver leaves it off.
    explicit PlainNames(const Driver* driver);
    ~PlainNames();

    bool clashed() const;

    /// Registers that @p gid renders as @p sym and reports whether the plain @p sym may be used.
    /// Sets the clash flag - but still answers `true` - if another gid already claimed @p sym.
    static bool claim(const Driver&, Sym sym, uint32_t gid);

    /// Is a diagnostic being formatted on @p driver right now?
    static bool active(const Driver&);

private:
    const Driver* driver_;
};

/// Renders a diagnostic through PlainNames and - if that turned out ambiguous - once more with Def::unique_name.
class Diag : public fe::CodeDiag {
public:
    explicit Diag(const Driver& driver)
        : driver_(driver) {}

    std::string render(const std::function<std::string()>&) const override;

private:
    const Driver& driver_;
};

/// Some "global" variables needed all over the place.
/// Well, there are not really global - that's the point of this class.
class Driver : public fe::Driver {
public:
    /// @name Construction
    ///@{
    Driver(std::string name = {});

    Driver(const Driver&)     = delete;
    Driver(Driver&&)          = delete;
    Driver& operator=(Driver) = delete;
    ///@}

    /// @name Getters
    ///@{
    Flags& flags() { return flags_; }
    const Flags& flags() const { return flags_; }
    fe::Log& log() { return log_; }
    const fe::Log& log() const { return log_; }
    fe::Profiler& profiler() { return profiler_; }
    const fe::Profiler& profiler() const { return profiler_; }
    World& world() { return world_; }
    const Version& version() const { return version_; } ///< MimIR Version.
    ///@}

    /// @name Diagnostic Naming
    /// Scratch state for PlainNames: which plain Def::sym each gid claimed while one message is formatted.
    /// It lives here - not in a global - so that concurrent Driver%s never share it.
    ///@{
    struct Names {
        size_t depth = 0;
        bool clashed = false;
        absl::flat_hash_map<Sym, u32> sym2gid;
    };

    Names& names() const { return names_; }
    ///@}

    /// An ordered list of directories.
    class Paths {
    public:
        void add(fs::path path) {
            if (fs::exists(path) && fs::is_directory(path)) paths_.insert(insert_, std::move(path));
        }

        /// Later Paths::add calls insert in front of everything added so far.
        void seal() { insert_ = paths_.begin(); }

        auto begin() const { return paths_.cbegin(); }
        auto end() const { return paths_.cend(); }

    private:
        std::list<fs::path> paths_;
        std::list<fs::path>::iterator insert_ = paths_.end();
    };

    /// @name Manage Search Paths
    /// A *plain directory* is probed as-is; a *prefix root* stands for an install tree and derives
    /// `<root>/<libdir>/mim` (plugins), `<root>/<datadir>/mim` (imports), and `<root>/<libdir>/mim/rt` (runtimes).
    /// Each lookup starts with the empty path, which probes the current working directory without an absolute path.
    /// Within a list, paths added first are searched first; CLI paths precede the environment and derived ones.
    ///@{
    void add_plugin_path(fs::path path) { plugin_dirs_.add(std::move(path)); }
    void add_import_path(fs::path path) { import_dirs_.add(std::move(path)); }
    void add_prefix_path(fs::path path) { prefixes_.add(std::move(path)); }

    /// Where Driver::load looks for `libmim_<name>`.
    fe::Vector<fs::path> plugin_paths() const;
    /// Where ast::Parser looks for `<name>.mim`; plugin directories are included, as a plugin ships both halves.
    fe::Vector<fs::path> import_paths() const;
    /// Where a backend looks for its runtime modules.
    fe::Vector<fs::path> rt_paths() const;
    ///@}

    /// @name Manage Imports
    /// Tracks the distinct import or plugin directives that World::dump should emit again later.
    ///@{
    class Imports {
    public:
        struct Entry {
            const fe::Src* src;
            Sym sym;
            ast::Tok::Tag tag;
            bool path; ///< The directive spelled a path (`import "a/b.mim"`) rather than a name.
        };

        /// @name Get imports
        ///@{
        const auto& entries() const { return entries_; }
        ///@}

        /// @name Iterators
        ///@{
        auto begin() const { return entries_.cbegin(); }
        auto end() const { return entries_.cend(); }
        ///@}

        /// Remembers the directive that pulled in @p src; a repeated import of the same file adds nothing.
        void add(const fe::Src* src, Sym, ast::Tok::Tag, bool path);

    private:
        std::deque<Entry> entries_;
    };

    const Imports& imports() const { return imports_; }
    Imports& imports() { return imports_; }
    ///@}

    /// @name Load Plugin
    /// Finds and loads a shared object file that implements the MimIR Plugin @p name.
    /// If \a name is an absolute path to a `.so`/`.dll` file, this is used.
    /// Otherwise, "name", "libmim_name.so" (Linux, Mac), "mim_name.dll" (Win)
    /// are searched for in Driver::search_paths().
    ///@{
    void load(std::string_view name);
    bool is_loaded(std::string_view name) const { return fe::lookup(plugins_, name); }
    /// Directory `libmim_<name>` was loaded from, so that its `<name>.mim` half cannot come from elsewhere.
    const fs::path* plugin_dir(std::string_view name) const { return fe::lookup(plugin2dir_, name); }
    void* get_fun_ptr(std::string_view plugin, const char* name);

    template<class F>
    auto get_fun_ptr(std::string_view plugin, const char* name) {
        return reinterpret_cast<F*>(get_fun_ptr(plugin, name));
    }
    ///@}

    /// @name Manage Plugins
    /// All these lookups yield `nullptr` if the key has not been found.
    ///@{
    auto phase(flags_t flags) { return fe::lookup(phases_, flags); }
    const auto& phases() const { return phases_; }
    auto normalizer(flags_t flags) const { return fe::lookup(normalizers_, flags); }
    auto normalizer(plugin_t d, tag_t t, sub_t s) const { return normalizer(Annex::flags(d, t, s)); }
    ///@}

    /// @name Plugin/Phase Arguments
    /// Freeform command-line arguments addressed to a plugin/phase (`-X <plugin>:<arg>`).
    /// A Phase reads its own arguments via Phase::args().
    ///@{
    void add_arg(std::string_view plugin, std::string arg) { plugin_args_[plugin].emplace_back(std::move(arg)); }
    /// Yields an empty fe::Vector if @p plugin has none.
    const fe::Vector<std::string>& args(std::string_view plugin) const;

    /// The PluginArg%s each loaded Plugin declares, in load order; only for listing them, see PluginArg.
    const auto& known_args() const { return known_args_; }

    /// The PluginEnv%s each loaded Plugin declares, in load order; only for listing them, see PluginEnv.
    const auto& known_envs() const { return known_envs_; }
    ///@}

private:
    // This must go *first* so plugins will be unloaded *last* in the d'tor; otherwise funny things might happen ...
    absl::node_hash_map<std::string, Plugin::Handle> plugins_;
    Version version_;
    Flags flags_;
    fe::Log log_;
    mutable Names names_;
    fe::Profiler profiler_;
    World world_;
    Paths plugin_dirs_, import_dirs_, prefixes_;
    absl::flat_hash_map<std::string, fs::path> plugin2dir_;
    Flags2Phases phases_;
    Normalizers normalizers_;
    absl::flat_hash_map<std::string, fe::Vector<std::string>> plugin_args_;
    std::vector<std::pair<std::string, fe::View<PluginArg>>> known_args_;
    std::vector<std::pair<std::string, fe::View<PluginEnv>>> known_envs_;
    Imports imports_;
};

#define GET_FUN_PTR(plugin, f) get_fun_ptr<decltype(f)>(plugin, #f)

} // namespace mim
