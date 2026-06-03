#pragma once

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Lowers the low-level tensor axioms (`map_reduce`, `get`, `set`, `broadcast`)
/// directly to their underlying primitives (loops, `extract`, `insert`, `pack`, …).
/// High-level axioms like `broadcast_in_dim` or `product_2d` are expected to have
/// been desugared to these low-level axioms by an earlier `Lower` phase.
class LowerMapReduce : public RWPhase {
public:
    LowerMapReduce(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    const Def* lower_get(const App*);
    const Def* lower_set(const App*);
    const Def* lower_broadcast(const App*);
    const Def* lower_map_reduce(const App*);

    const Def* rec_broadcast(const Def* s_in, const Def* s_out, const Def* input, u64 r, u64 i);
};

} // namespace mim::plug::tensor::phase
