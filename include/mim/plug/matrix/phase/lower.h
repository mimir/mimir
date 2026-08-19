#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix::phase {

/// Lowers the high-level matrix axioms (`product_2d`, `transpose_2d`, `sum`) into the low-level
/// `map_reduce_idx` axiom.
/// Each high-level axiom comes with a matching `*_impl` annex (a `fun` with the same signature as the axiom);
/// the lowering simply re-applies the args to the `_impl` annex.
/// The resulting low-level axiom is then lowered to `affine.For` loops by `LowerMapReduceIdx`.
/// This is the buffer-world counterpart of `tensor::phase::Lower`; some of these transformations could be done
/// as a normalizer.
class Lower : public RWPhase {
public:
    Lower(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::matrix::phase
