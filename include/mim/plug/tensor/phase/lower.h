#pragma once

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Lowers the high-level tensor axioms into the low-level tensor axioms (`map_reduce`,
/// `map_reduce_aff`, …). Each high-level axiom comes with a matching `*_impl` annex (a
/// `lam` with the same signature as the axiom); the lowering simply re-applies the args
/// to the `_impl` annex. Each `_impl` body references the `_impl` variants of its
/// dependencies, so the chain of beta-reductions bottoms out at the low-level axioms in
/// one step. The resulting low-level axioms are then lowered to primitives by
/// `LowerMapReduce`.
class Lower : public RWPhase {
public:
    Lower(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_via_impl(const App*, const Def* impl_annex);
};

} // namespace mim::plug::tensor::phase
