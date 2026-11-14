#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Makes each continuation's `mem.M 0` chain total over its instructions, in a topological order.
/// Memory instructions are rethreaded onto the chain; all others are wrapped in `x86.sched`.
/// Every use of a memory token outside an instruction receives the end of the chain.
class Schedule : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    const Def* rewrite_mut_Lam(Lam*) final;

    /// Appends the instructions @p def depends on to `order_` in post-order, stopping at mutables and scheduled Defs.
    void visit(const Def* def);

    DefSet visited_;
    DefVec order_;
    DefVec mems_;
};

} // namespace mim::plug::x86::phase
