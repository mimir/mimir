#pragma once

#include <string_view>

#include <mim/util/types.h>

namespace mim {
class Def;
class World;
} // namespace mim

/// The closed rewrite rules of the `x86_isel` plugin as C++; `x86_isel.gen` writes them to `isel_rules.h`.
namespace mim::plug::x86::rules {

#define MIM_X86_RULE_WANTS(X) X(Param) X(Lit) X(Reg) X(Imm) X(Sh) X(Base) X(Index) X(Addr) X(Scaled)

/// What a rule wants of the term a meta variable matches; a Param is bound but no operand.
/// A Lit is a Param that must be a literal.
/// Reg is a register, Base/Index an `x86.Base`/`x86.Index`, Addr an `x86.Addr i` for any `i`, and Scaled an index with
/// its scale. Imm is a literal that fits a sign-extended `i32` immediate, Sh a literal shift count, which x86 masks.
enum class Want : u8 {
#define X(w) w,
    MIM_X86_RULE_WANTS(X)
#undef X
};

#define MIM_X86_RULE_TYS(X) X(Reg) X(Base) X(Index) X(Scaled) X(Lit)

/// The non-terminal a rule yields, by the type of its rhs: a register, `x86.Base`, `x86.Index`, or an index with its
/// scale `[I64, I2]`. Lit is no rule's type but that of a literal operand.
enum class Ty : u8 {
#define X(t) t,
    MIM_X86_RULE_TYS(X)
#undef X
};

/// Matches the root @p def with the commuting nodes in @p swap swapped and binds @p metas and @p refs.
using MatchFn = bool (*)(const Def* def, u32 swap, const Def** metas, const Def** refs);
/// Builds the rhs from @p holes, one per meta variable, already in the new world.
using BuildFn = const Def* (*)(World&, const Def* const* holes);

/// How the lhs refers to one of its inner nodes.
enum class Ref : u8 {
    Value, ///< As an operand of its owner, which nothing outside the match uses.
    Mem,   ///< By its memory, which only the match uses.
};

/// A ref of a matcher: its cover and its owner, the node it is an operand of (0 for the root, `k + 1` for cover `k`).
struct RefInfo {
    u8 cover;
    u8 owner;
    Ref kind;
};

/// An operand position of a matcher in preorder: meta variable or Value ref @p index of the node @p owner.
struct Use {
    u8 owner;
    u8 index;
    bool ref;
};

/// A matcher and the shape of its lhs; a cover's operands follow it in preorder.
struct Matcher {
    MatchFn fn;
    u32 uses;
    u8 num_uses;
    u32 refs;
    u8 num_refs;
    u8 num_covers;
};

/// A closed rule `lhs ↦ rhs`, whose metas want `num_metas` consecutive entries of `wants`.
struct Rewrite {
    std::string_view name;
    u32 cost;     ///< The number of x86 instructions in its rhs.
    flags_t root; ///< The axiom at the head of its lhs, or 0 for an Extract.
    Ty type;      ///< The non-terminal its rhs yields.
    const Matcher* match;
    BuildFn build;
    u32 wants;
    u8 num_metas;
    u32 swaps; ///< The swaps of `match` that may yield another match.
    u8 maps;   ///< `k + 1` if the rhs replaces cover `k` as a whole, or 0.
};

/// A conversion rule `x ↦ rhs`, whose lhs is all of its meta variables: a value of non-terminal `from` as one of `to`.
/// Its builder takes the `arity` projections of the value.
struct Conversion {
    std::string_view name;
    u32 cost;
    Ty from, to;
    u8 arity;
    BuildFn build;
};

} // namespace mim::plug::x86::rules
