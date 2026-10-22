#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"
#include "mim/plug/gpu/phase/split_off_kernels.h"

namespace mim::plug::gpu::phase {

class SplitApply : public RWPhase {
public:
    using RWPhase::RWPhase;

    void start() final;

private:
    SplitOffKernels split_phase{old_world(), "splitoff_kernels_in_split_apply"};
};

} // namespace mim::plug::gpu::phase
