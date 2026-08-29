#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

/// Elides a `%gpu.deinit` followed by a `%gpu.init`, e.g. between two back-to-back `LowerMapReduce` launches.
/// The whole program shares one fatbinary, so the pair collapses to `%gpu.deinit`'s `(mem, GlobalM, ConstM)` operands.
class MergeInitDeinit : public RWPhase {
public:
    using Super = RWPhase;

    MergeInitDeinit(World& world, flags_t annex)
        : Super(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::gpu::phase
