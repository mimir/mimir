#pragma once

#include <mim/phase.h>

namespace mim::plug::gpu::phase {

class LowerMapReduce : public RWPhase {
public:
    using Super = RWPhase;
    using RWPhase::RWPhase;

private:
    /// Skips the whole phase if the program already contains an explicit `gpu.init`.
    // TODO: consider different solution to gpu.init vs gpu.auto_init problem
    void start() final;
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_map_reduce_post(const App*);
};

} // namespace mim::plug::gpu::phase
