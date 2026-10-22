#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class MemChecks : public Analysis {
public:
    using Super = Analysis;

    using Analysis::Analysis;

private:
    const Def* rewrite_imm_App(const App*) final;
    void rewrite_external(Def*) final;
};

} // namespace mim::plug::gpu::phase
