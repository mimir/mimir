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
    Driver(std::string name);
    Driver()
        : Driver(std::string{}) {}

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

    /// @name Manage Search Paths
    /// Search paths for plugins are in the following order:
    /// 1. The empty path. Used as prefix to look into current working directory without resorting to an absolute path.
    /// 2. All further user-specified paths via Driver::add_search_path; paths added first will also be searched first.
    /// 3. All paths specified in the environment variable `MIM_PLUGIN_PATH`.
    /// 4. The path derived from the location of `libmim` (`<libmim>/mim`)
    /// 5. `CMAKE_INSTALL_PREFIX/lib/mim`
    ///@{
    const auto& search_paths() const { return search_paths_; }
    void add_search_path(fs::path path) {
        if (fs::exists(path) && fs::is_directory(path)) search_paths_.insert(insert_, std::move(path));
    }
    ///@}

    /// @name Manage Imports
    /// This tracks:
    /// 1. The distinct files that have already been parsed to avoid reparsing them,
    /// 2. The distinct import or plugin directives that should be emitted again later.
    ///@{
    class Imports {
    public:
        struct Entry {
            const fe::Src* src;
            Sym sym;
            ast::Tok::Tag tag;
        };

        Imports(Driver& driver)
            : driver_(driver) {}

        /// @name Get imports
        ///@{
        const auto& entries() const { return entries_; }
        ///@}

        /// @name Iterators
        ///@{
        auto begin() const { return entries_.cbegin(); }
        auto end() const { return entries_.cend(); }
        ///@}

        /// Reads @p path, remembers the import or plugin directive, and reports whether the file is new.
        /// Yields a `nullptr` fe::Src if @p path cannot be read; reporting that is the caller's job,
        /// since only it knows the Loc of the directive to blame.
        std::pair<const fe::Src*, bool> add(fs::path, Sym, ast::Tok::Tag);

    private:
        Driver& driver_;
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
    const fe::Vector<std::string>& args(std::string_view plugin) const; ///< Empty fe::Vector if @p plugin has none.

    /// The PluginArg%s each loaded Plugin declares, in load order; only for listing them, see PluginArg.
    const auto& known_args() const { return known_args_; }
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
    std::list<fs::path> search_paths_;
    std::list<fs::path>::iterator insert_ = search_paths_.end();
    Flags2Phases phases_;
    Normalizers normalizers_;
    absl::flat_hash_map<std::string, fe::Vector<std::string>> plugin_args_;
    std::vector<std::pair<std::string, fe::View<PluginArg>>> known_args_;
    Imports imports_;
};

#define GET_FUN_PTR(plugin, f) get_fun_ptr<decltype(f)>(plugin, #f)

} // namespace mim
