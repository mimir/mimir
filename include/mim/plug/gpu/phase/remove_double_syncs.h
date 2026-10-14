#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class RemoveDoubleSyncs : public RWPhase {
public:
    using Super = RWPhase;

    using RWPhase::RWPhase;

private:
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::gpu::phase
