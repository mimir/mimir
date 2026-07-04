#pragma once

#include <mim/def.h>
#include <mim/pass.h>
#include <mim/phase.h>

namespace mim::plug::matrix {

/// In this phase, we lower all matrix operations and types to the low-level representation using pointers.
/// The matrix type is replaced by a pointer to n nested arrays.
/// - `init` is replaced with `alloc`
/// - `read` becomes `lea+load`
/// - `insert` becomes `lea+store`
/// - `constMat` becomes `alloc+pack+store`

class LowerMatrixLowLevel : public RWPhase {
public:
    LowerMatrixLowLevel(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) override;

private:
    Def2Def rewritten;
};

} // namespace mim::plug::matrix
