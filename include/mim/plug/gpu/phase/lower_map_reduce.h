#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class LowerMapReduce : public RWPhase {
public:
    using Super = RWPhase;

    LowerMapReduce(World& world, flags_t annex)
        : Super(world, annex) {}

private:
    /// Skips the whole phase if the program already contains an explicit `%gpu.init`: automatic and
    /// user-managed GPU sessions can't currently cooperate, so `%btensor.map_reduce_post` is left for
    /// `%btensor.lower_map_reduce`'s ordinary CPU lowering instead.
    // TODO: consider different solution to %gpu.init vs %gpu.auto_init problem
    void start() final;
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_map_reduce_post(const App*);
};

} // namespace mim::plug::gpu::phase
