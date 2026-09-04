#pragma once

#include <compare>

#include <functional>
#include <initializer_list>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include <absl/container/flat_hash_map.h>

#include "mim/config.h"
#include "mim/def.h"

namespace mim {

class Driver;
class Phase;

/// @name Plugin Interface
///@{
using Normalizers = absl::flat_hash_map<flags_t, NormalizeFn>;

/// Maps an axiom of a Phase to a function that creates one.
using Flags2Phases = absl::flat_hash_map<flags_t, std::function<std::unique_ptr<Phase>(World&)>>;

/// One `-X <plugin>:<arg>` a Plugin understands; see @ref clipluginargs.
/// A Plugin declares these next to the code that picks them apart, so that `mim -p <plugin> -h` can list them.
struct PluginArg {
    const char* syntax; ///< How to spell the argument, e.g. `"o=<file>, output=<file>"`.
    const char* descr;  ///< What it does; one sentence, Markdown.
};

/// One environment variable a Plugin reads; see @ref clipluginenv.
/// A Plugin declares these next to the code that reads them, so that `mim -p <plugin> -h` can list them.
struct PluginEnv {
    const char* name;  ///< Name of the variable, e.g. `"CUDA_HOME"`.
    const char* descr; ///< What it does; one sentence, Markdown.
};
///@}

/// @name Plugin Argument Lookup
/// Picks the `-X <plugin>:<arg>` strings of Driver::args / Phase::args apart.
/// Each helper matches any of @p keys - `arg_value(args(), "o", "output")` - and the last occurrence wins.
///@{
namespace detail {
/// `<key>` ↦ `""`, `<key>=<value>` ↦ `<value>`, anything else ↦ `std::nullopt`.
inline std::optional<std::string_view> arg_split(std::string_view arg, std::string_view key) {
    if (!arg.starts_with(key)) return {};
    auto val = arg.substr(key.size());
    if (val.empty()) return val;
    if (val.front() == '=') return val.substr(1);
    return {};
}
} // namespace detail

/// Value of `<key>=<value>`; `std::nullopt` if none of @p keys carries one.
template<class... Keys>
std::optional<std::string_view> arg_value(fe::View<std::string> args, Keys... keys) {
    std::optional<std::string_view> res;
    for (std::string_view arg : args)
        for (std::string_view key : {std::string_view(keys)...})
            if (auto val = detail::arg_split(arg, key); val && !val->empty()) res = val;
    return res;
}

/// An @p on key ↦ `true`, an @p off key ↦ `false`; `std::nullopt` if neither occurs.
inline std::optional<bool> arg_bool(fe::View<std::string> args,
                                    std::initializer_list<std::string_view> on,
                                    std::initializer_list<std::string_view> off) {
    std::optional<bool> res;
    for (std::string_view arg : args) {
        for (auto key : on)
            if (arg == key) res = true;
        for (auto key : off)
            if (arg == key) res = false;
    }
    return res;
}

/// Whether any of @p keys occurs.
template<class... Keys>
bool arg_flag(fe::View<std::string> args, Keys... keys) {
    for (std::string_view arg : args)
        for (std::string_view key : {std::string_view(keys)...})
            if (arg == key) return true;
    return false;
}
///@}

struct Version {
    int major;
    int minor;
    const char* suffix;
    const char* hash;

    /// Compares major/minor/suffix, ignores hash.
    constexpr auto operator<=>(const Version& other) const noexcept {
        auto cmp = std::tie(major, minor) <=> std::tie(other.major, other.minor);
        if (cmp != 0) return cmp;

        return std::strcmp(suffix, other.suffix) <=> 0;
    }

    /// Compares major/minor/suffix, ignores hash.
    constexpr bool operator==(const Version& other) const noexcept {
        return major == other.major && minor == other.minor && std::strcmp(suffix, other.suffix) == 0;
    }

    friend std::ostream& operator<<(std::ostream& os, const Version& v) {
        return os << v.major << '.' << v.minor << v.suffix << " (" << v.hash << ")";
    }
};

extern "C" {

#define MIM_VERSION \
    Version { MIM_VER_MAJOR, MIM_VER_MINOR, MIM_VER_SUFFIX, MIM_GIT_HASH }

/// Basic info and registration function pointer to be returned from a specific plugin.
/// Use Driver to load such a plugin.
struct Plugin {
    using Handle = std::unique_ptr<void, void (*)(void*)>;

    const char* name; ///< Name of the Plugin.
    Version version;  ///< Version of the Plugin.

    /// Callback for registering the mapping from axm ids to normalizer functions in the given @p normalizers map.
    void (*register_normalizers)(Normalizers&);
    /// Callback for registering the Plugin's callbacks for Phase%s.
    void (*register_phases)(Flags2Phases&);

    // No default member initializers: only a POD is C-compatible as an `extern "C"` return type.
    const PluginArg* args; ///< The `-X` arguments this Plugin understands; see PluginArg.
    size_t num_args;       ///< Number of Plugin::args.
    const PluginEnv* envs; ///< The environment variables this Plugin reads; see PluginEnv.
    size_t num_envs;       ///< Number of Plugin::envs.
};

/// @name Plugin Interface
/// @see Plugin
///@{
/// To be implemented and exported by a plugin.
/// @returns a filled Plugin.
MIM_EXPORT mim::Plugin mim_get_plugin();
///@}
}

/// Holds info about an entity defined within a Plugin (called *Annex*).
struct Annex {
    Annex() = delete;

    /// @name Mangling Plugin Name
    ///@{
    static constexpr size_t Max_Plugin_Size = 8;
    static constexpr plugin_t Global_Plugin = 0xffff'ffff'ffff'0000_u64;

    /// Mangles @p s into a dense 48-bit representation.
    /// The layout is as follows:
    /// ```
    /// |---7--||---6--||---5--||---4--||---3--||---2--||---1--||---0--|
    /// 7654321076543210765432107654321076543210765432107654321076543210
    /// Char67Char66Char65Char64Char63Char62Char61Char60|---reserved---|
    /// ```
    /// The `reserved` part is used for the Axm::tag and the Axm::sub.
    /// Each `Char6x` is 6-bit wide and hence a plugin name has at most Axm::Max_Plugin_Size = 8 chars.
    /// It uses this encoding:
    /// | `Char6` | ASCII   |
    /// |---------|---------|
    /// | 1:      | `_`     |
    /// | 2-27:   | `a`-`z` |
    /// | 28-53:  | `A`-`Z` |
    /// | 54-63:  | `0`-`9` |
    /// The 0 is special and marks the end of the name if the name has less than 8 chars.
    /// @returns `std::nullopt` if encoding is not possible.
    static std::optional<plugin_t> mangle(std::string_view plugin);

    /// Reverts an Axm::mangle%d @p plugin back to its name; never longer than Annex::Max_Plugin_Size.
    /// Ignores lower 16-bit of @p plugin.
    static std::string demangle(plugin_t plugin);

    static std::tuple<Sym, Sym, Sym> split(Driver&, Sym);
    ///@}

    /// @name Annex Name
    /// @anchor annex_name
    /// Anatomy of an Annex name:
    /// ```
    /// %plugin.tag.sub
    /// |  48  | 8 | 8 | <-- Number of bits per field.
    /// ```
    /// * Def::name() retrieves the full name as Sym.
    /// * Def::flags() retrieves the full name as Axm::mangle%d 64-bit integer.
    ///@{
    /// Yields the `plugin` part of the name as integer.
    /// It consists of 48 relevant bits that are returned in the highest 6 bytes of a 64-bit integer.
    static constexpr plugin_t flags2plugin(flags_t f) { return f & Global_Plugin; }

    /// Yields the `tag` part of the name as integer.
    static constexpr tag_t flags2tag(flags_t f) { return tag_t((f & 0x0000'0000'0000'ff00_u64) >> 8_u64); }

    /// Yields the `sub` part of the name as integer.
    static constexpr sub_t flags2sub(flags_t f) { return sub_t(f & 0x0000'0000'0000'00ff_u64); }

    /// Includes Axm::plugin() and Axm::tag() but **not** Axm::sub.
    static constexpr flags_t flags2base(flags_t f) { return f & ~0xff_u64; }

    /// Assembles the full flags from its `plugin`, `tag`, and `sub` fields.
    static constexpr flags_t flags(plugin_t p, tag_t t, sub_t s = 0) { return p | (flags_t(t) << 8_u64) | flags_t(s); }
    ///@}

    /// @name Helpers for Matching
    /// These are set via template specialization.
    ///@{
    // clang-format off
    template<class Id> static constexpr size_t  Num  =  size_t(-1); ///< Number of Axm::sub%tags.
    template<class Id> static constexpr flags_t Base = flags_t(-1); ///< @see Axm::base.
    template<class Id> static consteval size_t  num () { return Num <Id>; }
    template<class Id> static consteval flags_t base() { return Base<Id>; }
    // clang-format of
    ///@}
};

} // namespace mim

#ifndef DOXYGEN
template<> struct std::formatter<mim::Version> : fe::ostream_formatter {};
#endif
