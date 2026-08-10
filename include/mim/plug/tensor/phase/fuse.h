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
    const Def* fuse_epilogue(const App* callee, const Def* arg);

    /// Old-world consumer count per `map_reduce_post` app (attributed through tuple wrappers), for
    /// the epilogue direction's single-consumer guard.
    DefMap<u64> mr_consumers_;
    /// Rewritten/fused `map_reduce_post` app → the old-world app it replaces, to look up the
    /// consumer count of new-world fusion candidates.
    DefMap<const Def*> new2old_;
};

} // namespace mim::plug::tensor::phase
