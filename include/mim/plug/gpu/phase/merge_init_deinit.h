#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

/// Elides a `%gpu.deinit` immediately followed by a `%gpu.init` on its result — tearing down a context
/// only to recreate an equivalent one, e.g. between two `LowerMapReduce`-generated launches back to back.
/// Since the whole program shares one fatbinary, the next launch's kernel is already resolvable from the
/// still-loaded module, so the pair collapses to `%gpu.deinit`'s own `(mem, GlobalM, ConstM)` operands.
class MergeInitDeinit : public RWPhase {
public:
    using Super = RWPhase;

    MergeInitDeinit(World& world, flags_t annex)
        : Super(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::gpu::phase
