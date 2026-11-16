#pragma once

#include <optional>

#include <mim/def.h>

namespace mim::plug::x86 {

/// What an instruction on the chain prescribes about the registers of its operands.
/// Registers are the `I4` literals `x86.rax` etc.
struct Constraint {
    DefVec ops;                                      ///< See operands().
    fe::Vector<std::pair<size_t, const Def*>> fixed; ///< Non-literal operands that have to be in a given register.
    std::optional<size_t> tied;                      ///< The operand whose register the (only) result takes.
    fe::Vector<const Def*> clobbers;                 ///< Registers written while the operands still occupy theirs.
};

/// The operands of @p insn: the argument of a `sched`'s instruction, otherwise the arguments including the memory.
DefVec operands(const App* insn);
Constraint constraint(const App* insn);
/// @p insn continuing @p m with @p ops instead of its operands.
const App* rebuild(const App* insn, const Def* m, const DefVec& ops);

/// The fixed operands with their registers, and the tied one with `nullptr` if it is @p live_after.
inline fe::Vector<std::pair<size_t, const Def*>> copies(const Constraint& c, auto live_after) {
    auto res = c.fixed;
    if (c.tied && live_after(c.ops[*c.tied])) res.emplace_back(*c.tied, nullptr);
    return res;
}

} // namespace mim::plug::x86
