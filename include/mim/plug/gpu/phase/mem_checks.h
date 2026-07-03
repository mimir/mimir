#pragma once

#include "mim/phase.h"

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class MemChecks : public Analysis {
public:
    using Super = Analysis;

    MemChecks(World& world, flags_t annex)
        : Analysis(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;
    void rewrite_external(Def*) final;

    DefSet analyzed_;
    LamSet kernels_;
};

} // namespace mim::plug::gpu::phase
