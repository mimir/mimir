#pragma once

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Lowers the high-level tensor axioms into the low-level tensor axioms (`map_reduce`,
/// `broadcast`, …). For axioms that come with a matching `*_impl` annex (a `lam` with
/// the same signature as the axiom) the lowering simply re-applies the args to the
/// `_impl` annex; each `_impl` body references the `_impl` variants of its
/// dependencies, so the chain of beta-reductions bottoms out at the low-level axioms
/// in one step. `broadcast_in_dim` does not have an `_impl` counterpart and is
/// desugared directly. The resulting low-level axioms are then expected to be
/// lowered to primitives by `LowerMapReduce`.
class Lower : public RWPhase {
public:
    Lower(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_broadcast_in_dim(const App*);
    const Def* lower_via_impl(const App*, const Def* impl_annex);
};

} // namespace mim::plug::tensor::phase
