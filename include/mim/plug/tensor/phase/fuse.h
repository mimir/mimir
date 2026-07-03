#pragma once

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

class Fuse : public RWPhase {
public:
    Fuse(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    const Def* fuse_map_reduce(const App*);
};

} // namespace mim::plug::tensor::phase
