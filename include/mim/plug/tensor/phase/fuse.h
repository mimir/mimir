#pragma once

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

class Fuse : public RWPhase {
public:
    Fuse(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void start() override;
    const Def* rewrite_imm_App(const App*) final;

    const Def* fuse_map_reduce(const App*);
    const Def* fuse_epilogue(const App*);

    /// Old-world user count per Def, for the epilogue direction's single-consumer guard.
    DefMap<u64> num_users_;
};

} // namespace mim::plug::tensor::phase
