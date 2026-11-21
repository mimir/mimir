#pragma once

#include <string_view>

#include <mim/util/types.h>

namespace mim {
class Def;
class World;
} // namespace mim

/// The closed rewrite rules of the `x86_isel` plugin as C++; `x86_isel.gen` writes them to `isel_rules.h`.
namespace mim::plug::x86::rules {

#define MIM_X86_RULE_WANTS(X) X(Param) X(Reg) X(Imm) X(Sh) X(Base) X(Index) X(Addr)

/// What a rule wants of the term a meta variable matches; a Param is bound but no operand.
enum class Want : u8 {
#define X(w) w,
    MIM_X86_RULE_WANTS(X)
#undef X
};

/// Matches the root @p def, with the two operands of its argument swapped if @p swap is set, and binds @p metas.
using MatchFn = bool (*)(const Def* def, bool swap, const Def** metas);
/// Builds the rhs from @p holes, one per meta variable, already in the new world.
using BuildFn = const Def* (*)(World&, const Def* const* holes);

/// A closed rule `lhs ↦ rhs`, whose metas want `num_metas` consecutive entries of `wants`.
struct Rewrite {
    std::string_view name;
    u32 cost;     ///< The number of x86 instructions in its rhs.
    flags_t root; ///< The axiom at the head of its lhs.
    flags_t head; ///< The axiom at the head of its rhs, or 0.
    MatchFn match;
    BuildFn build;
    u32 wants;
    u8 num_metas;
};

} // namespace mim::plug::x86::rules
