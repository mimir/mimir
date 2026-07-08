#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix {

/// Lowers the affine-indexed buffer-world operations (`%matrix.map_reduce_aff`, `%matrix.broadcast`) into
/// `affine.For` loop nests over `%buffer.read` / `%buffer.write` / `%buffer.alloc`, threading `%mem.M`.
/// These are the buffer-world counterparts of `%tensor.map_reduce_aff` / `%tensor.broadcast`; the `tensor`
/// plugin's bufferization (`%tensor.lower_to_mem`) maps the SSA tensor ops onto them.
class LowerAff : public RWPhase {
public:
    LowerAff(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) override;
    const Def* lower_map_reduce_aff(const App*);
    const Def* lower_broadcast(const App*);

    /// Drops, from the (unfolded) index `idx`, the components of size-1 dimensions of `shape`
    /// (MimIR folds size-1 dims out of buffer types).
    const Def* fold_index(const Def* shape, const Def* idx);
};

} // namespace mim::plug::matrix
