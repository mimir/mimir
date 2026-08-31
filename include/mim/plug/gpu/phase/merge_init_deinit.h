#pragma once

#include <mim/phase.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

/// Collapses every `%gpu.init`/`%gpu.deinit` reachable from one external function down to exactly one real
/// pair: the first `%gpu.init` the rewrite reaches stays real and its `(GlobalM, ConstM)` is cached, every
/// other `%gpu.init`/`%gpu.deinit` for that function is eliminated (a pure mem pass-through reusing that
/// cache), and one real `%gpu.deinit` is inserted right before each of the function's own return points.
/// By the time this phase runs, `%mem.seo` has already linearized the whole program into one fixed
/// sequential order, so "the first `%gpu.init` the rewrite reaches" is also the first one that runs at
/// runtime - no data-dependency proof between the original call sites is needed, only that they share the
/// one process-wide CUDA context/module the whole program already renders down to.
class MergeInitDeinit : public RWPhase {
public:
    using Super = RWPhase;

    MergeInitDeinit(World& world, flags_t annex)
        : Super(world, annex) {}

private:
    void start() override;
    const Def* rewrite_imm_App(const App*) final;

    struct Session {
        const Def* global = nullptr;
        const Def* const_ = nullptr;
    };

    Vector<std::unique_ptr<Session>> sessions_;
    DefMap<Session*> init_session_;
    DefMap<Session*> deinit_session_;
    DefMap<Session*> ret_session_;
};

} // namespace mim::plug::gpu::phase
