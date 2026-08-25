#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::btensor::phase {

/// Lowers the buffer-world operations (`%btensor.map_reduce_post`, `%btensor.broadcast`, `%btensor.pad`,
/// `%btensor.concat`) into `affine.For` loop nests over `%buffer.read` / `%buffer.write` / `%buffer.alloc`,
/// threading `%mem.M`.
/// These are the buffer-world counterparts of the corresponding `%tensor.*` ops; the `tensor` plugin's
/// bufferization (`%tensor.lower_to_mem`) maps the SSA tensor ops onto them.
/// Also lowers `%buffer.constant` into a fill loop, so a large constant/splat tensor becomes a loop rather
/// than a monolithic `%mem.store` of a giant literal array (which the LLVM backend cannot digest).
class LowerMapReduce : public RWPhase {
public:
    LowerMapReduce(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) override;
    const Def* lower_map_reduce_post(const App*);
    const Def* lower_broadcast(const App*);
    const Def* lower_pad(const App*);
    const Def* lower_concat(const App*);
    const Def* lower_buffer_constant(const App*);
};

} // namespace mim::plug::btensor::phase
