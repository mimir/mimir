#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix {

/// Resolves lowering of high level operations into medium/other high-level operations.
/// Some of these transformations could be done as normalizer.
/// We rewrite matrix operations like sum, transpose, and product into `map_reduce` operations.
/// The corresponding `map_reduce` operations are annexes in the `matrix` plugin.

class LowerMatrixHighLevelMapRed : public RWPhase {
public:
    LowerMatrixHighLevelMapRed(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::matrix
