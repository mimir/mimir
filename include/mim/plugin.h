#pragma once

#include <compare>

#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <tuple>

#include <absl/container/btree_map.h>
#include <absl/container/flat_hash_map.h>

#include "mim/config.h"
#include "mim/def.h"

namespace mim {

class Driver;
class Stage;

/// @name Plugin Interface
///@{
using Normalizers = absl::flat_hash_map<flags_t, NormalizeFn>;

/// Maps an an axiom of a Stage to a function that creates one.
using Flags2Stages = absl::flat_hash_map<flags_t, std::function<std::unique_ptr<Stage>(World&)>>;

using Backends = absl::btree_map<std::string, void (*)(World&, std::ostream&)>;
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
    /// Callback for registering the Plugin's callbacks for Pass%es and Phase%s.
    void (*register_stages)(Flags2Stages&);
    /// Callback for registering the mapping from backend names to emission functions in the given @p backends map.
    void (*register_backends)(Backends&);
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
    static std::optional<plugin_t> mangle(Sym plugin);

    /// Reverts an Axm::mangle%d string to a Sym.
    /// Ignores lower 16-bit of @p u.
    static Sym demangle(Driver&, plugin_t plugin);

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
    static plugin_t flags2plugin(flags_t f) { return f & Global_Plugin; }

    /// Yields the `tag` part of the name as integer.
    static tag_t flags2tag(flags_t f) { return tag_t((f & 0x0000'0000'0000'ff00_u64) >> 8_u64); }

    /// Yields the `sub` part of the name as integer.
    static sub_t flags2sub(flags_t f) { return sub_t(f & 0x0000'0000'0000'00ff_u64); }

    /// Includes Axm::plugin() and Axm::tag() but **not** Axm::sub.
    static flags_t flags2base(flags_t f) { return f & ~0xff_u64; }
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
