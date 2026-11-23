#pragma once

#include <mim/phase.h>
#include <mim/schedule.h>
#include <mim/target.h>

namespace mim::plug::mem::phase {

/// Makes each continuation's `mem.M 0` chain total over the instructions of the Driver::target, in a topological order.
/// Memory instructions are rethreaded onto the chain; all others are wrapped in `mem.sched`.
/// Scheduler::smart picks the continuation of an instruction; a closed one is rematerialized right before each use.
/// Every use of a memory token outside an instruction receives the end of the chain.
/// No type changes, so each Nest is rewritten in place.
class Schedule : public NestPhase<Lam>, private Rewriter {
public:
    Schedule(World& world, flags_t annex)
        : NestPhase(world, annex, true)
        , Rewriter(world) {}

    using NestPhase::world;

private:
    void visit(const Nest&) final;
    const Def* rewrite(const Def*) final;
    const Def* rewrite_imm_App(const App*) final;
    void thread(Lam*);
    /// Maps the instructions of @p lam onto the chain that starts at its `i`th Var.
    void chain(Lam* lam, size_t i);

    /// Places the instructions and memory tokens @p def depends on, stopping at mutables.
    /// @p curr is the continuation of the user of @p def.
    void place(Lam* curr, const Def* def);

    bool is_insn(const Def* def) const { return def->isa<App>() && target_->is_insn(def->as<App>()); }
    /// An instruction without a place of its own: a fresh copy goes right before each use.
    bool is_remat(const Def* def) const { return is_insn(def) && (Scheduler::is_remat(def) || target_->is_flags(def)); }

    const Target* target_ = nullptr;
    Scheduler* scheduler_ = nullptr;
    DefSet placed_;
    LamSet threaded_;
    LamMap<DefSet> visited_;
    LamMap<DefVec> insns_; ///< Per continuation in a topological order; a closed instruction once per use.
    LamMap<DefVec> mems_;
};

} // namespace mim::plug::mem::phase
