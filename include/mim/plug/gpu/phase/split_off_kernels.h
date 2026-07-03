#pragma once

#include "mim/phase.h"

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

class SplitOffKernels : public RWPhase {
public:
    SplitOffKernels(World& world, flags_t annex)
        : RWPhase(world, annex) {}
    SplitOffKernels(World& world, std::string name)
        : RWPhase(world, std::move(name)) {}

private:
    void start() final;
    bool analyze() final;
    void analyze(const Def*);

    const Def* rewrite_mut_Lam(Lam*) final;

    DefSet analyzed_;
    LamSet kernels_;
};

} // namespace mim::plug::gpu::phase
