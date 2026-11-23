#pragma once

#include <mim/phase.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm::phase {

/// Rewrites the `core` and `mem` operations that `mem.legalize_native` left into ARM instructions.
/// Everything this phase does not know is an error.
class ISel : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    /// The `Flags` that hold the condition of the `Bool` @p cond, and the condition code that tests them.
    std::pair<const Def*, arm::cc> cond(const Def* cond);
};

} // namespace mim::plug::arm::phase
