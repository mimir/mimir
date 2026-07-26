#pragma once

#include <mim/phase.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

/// Wraps operands with `%clos.attr` markers (returning, free_bb, fstclass_bb, ...) and eta-expands
/// branches and continuations so that ClosConv sees a canonical program.
class ClosConvPrep : public RWPhase {
public:
    ClosConvPrep(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// Fills lam2fscope_: assigns each basicblock Lam to its enclosing returning Lam.
    bool analyze() final;
    const Def* rewrite_imm_App(const App*) final;

    const Def* rewrite_arg(const App* app, const Def* old_op);
    const Def* rewrite_callee_op(const Def* old_op);

    Lam* scope(Lam* lam) { return lam2fscope_[lam]; }

    bool from_outer_scope(Lam* lam) {
        auto mut = curr_mut() ? curr_mut()->isa_mut<Lam>() : nullptr;
        return mut && scope(lam) && scope(lam) != scope(mut);
    }

    const Def* eta_wrap(const Def* old_op, attr a);

    DefMap<Lam*> old2wrapper_;
    Lam2Lam lam2fscope_;
    bool analyzed_ = false;
};

} // namespace mim::plug::clos::phase
