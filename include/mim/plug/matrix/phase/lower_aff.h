#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix {

/// Lowers the buffer-world operations (`%matrix.map_reduce_aff`, `%matrix.broadcast`, `%matrix.pad`,
/// `%matrix.concat`) into `affine.For` loop nests over `%buffer.read` / `%buffer.write` / `%buffer.alloc`,
/// threading `%mem.M`.
/// These are the buffer-world counterparts of the corresponding `%tensor.*` ops; the `tensor` plugin's
/// bufferization (`%tensor.lower_to_mem`) maps the SSA tensor ops onto them.
class LowerAff : public RWPhase {
public:
    LowerAff(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) override;
    const Def* lower_map_reduce_aff(const App*);
    const Def* lower_broadcast(const App*);
    const Def* lower_pad(const App*);
    const Def* lower_concat(const App*);
};

} // namespace mim::plug::matrix
