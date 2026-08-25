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
    const Def* fuse_read_through(const App* callee, const Def* arg);
    bool sole_consumer(const Def*) const;

    /// Old-world consumer count per `map_reduce_post` app (attributed through tuple wrappers), for
    /// the epilogue direction's single-consumer guard.
    DefMap<u64> mr_consumers_;
    /// New-world defs whose consumers were multiplied by bypassing a shared (or uncounted) read;
    /// the old-world count cannot see this, so `sole_consumer` rejects them.
    DefSet shared_;
    /// Rewritten/fused `map_reduce_post` app → the old-world app it replaces, to look up the
    /// consumer count of new-world fusion candidates.
    DefMap<const Def*> new2old_;
};

} // namespace mim::plug::tensor::phase
