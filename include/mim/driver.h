#pragma once

#include <list>
#include <utility>

#include <absl/container/node_hash_map.h>

#include "mim/flags.h"
#include "mim/plugin.h"
#include "mim/world.h"

#include "mim/ast/tok.h"
#include "mim/util/log.h"
#include "mim/util/profile.h"

namespace mim {

/// Some "global" variables needed all over the place.
/// Well, there are not really global - that's the point of this class.
class Driver : public fe::SymPool {
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
    Log& log() const { return log_; }
    Profiler& profiler() { return profiler_; }
    const Profiler& profiler() const { return profiler_; }
    World& world() { return world_; }
    const Version& version() const { return version_; } ///< MimIR Version.
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
            fs::path path;
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

        /// Remembers an import or plugin directive and reports whether the resolved file is new.
        std::pair<const fs::path*, bool> add(fs::path, Sym, ast::Tok::Tag);

    private:
        Driver& driver_;
        std::deque<Entry> entries_;
        std::deque<fs::path> parsed_paths_;
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
    void load(Sym name);
    void load(const std::string& name) { return load(sym(name)); }
    bool is_loaded(Sym sym) const { return lookup(plugins_, sym); }
    void* get_fun_ptr(Sym plugin, const char* name);

    template<class F>
    auto get_fun_ptr(Sym plugin, const char* name) {
        return reinterpret_cast<F*>(get_fun_ptr(plugin, name));
    }

    template<class F>
    auto get_fun_ptr(const char* plugin, const char* name) {
        return get_fun_ptr<F>(sym(plugin), name);
    }
    ///@}

    /// @name Manage Plugins
    /// All these lookups yield `nullptr` if the key has not been found.
    ///@{
    auto stage(flags_t flags) { return lookup(stages_, flags); }
    const auto& stages() const { return stages_; }
    auto normalizer(flags_t flags) const { return lookup(normalizers_, flags); }
    auto normalizer(plugin_t d, tag_t t, sub_t s) const { return normalizer(Annex::flags(d, t, s)); }
    ///@}

private:
    // This must go *first* so plugins will be unloaded *last* in the d'tor; otherwise funny things might happen ...
    absl::node_hash_map<Sym, Plugin::Handle> plugins_;
    Version version_;
    Flags flags_;
    mutable Log log_;
    Profiler profiler_;
    World world_;
    std::list<fs::path> search_paths_;
    std::list<fs::path>::iterator insert_ = search_paths_.end();
    Flags2Stages stages_;
    Normalizers normalizers_;
    Imports imports_;
};

#define GET_FUN_PTR(plugin, f) get_fun_ptr<decltype(f)>(plugin, #f)

} // namespace mim
