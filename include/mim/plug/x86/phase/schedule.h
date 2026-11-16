#pragma once

#include <mim/phase.h>
#include <mim/schedule.h>

namespace mim::plug::x86::phase {

/// Makes each continuation's `mem.M 0` chain total over its instructions, in a topological order.
/// Memory instructions are rethreaded onto the chain; all others are wrapped in `x86.sched`.
/// Scheduler::smart picks the continuation of an instruction; a closed one is rematerialized right before each use.
/// Every use of a memory token outside an instruction receives the end of the chain.
class Schedule : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// The placement of all instructions within one Nest.
    struct Fun {
        explicit Fun(Lam* root)
            : nest(root)
            , scheduler(nest) {}

        Nest nest;
        Scheduler scheduler;
        DefSet placed;
        LamMap<DefSet> visited;
        LamMap<DefVec> insns; ///< Per continuation in a topological order; a closed instruction once per use.
        LamMap<DefVec> mems;
    };

    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* thread(Lam*);

    /// Places the instructions and memory tokens @p def depends on, stopping at mutables.
    /// @p curr is the continuation of the user of @p def.
    void place(Lam* curr, const Def* def);

    Fun* fun_ = nullptr;
};

} // namespace mim::plug::x86::phase
