#pragma once

#include <mim/def.h>
#include <mim/lam.h>

#include <mim/phase/ds2cps.h>

namespace mim::plug::tensor::phase {

/// Bufferizes nominal Tensor types and operations onto the shared buffer layer.
/// The following mem.add_mem phase threads the emitted memory operands.
class LowerToMem : public DS2CPS {
public:
    using DS2CPS::DS2CPS;

private:
    void start() override;
    void validate();
    bool is_tensor_type(const Def*) const;
    bool mentions_tensor(const Def*) const;
    bool is_tensor_fn(Lam*) const;

    const Def* rewrite(const Def*) override;
    const Def* rewrite_mut_Lam(Lam*) override;
    std::pair<const Def*, const Def*> rewrite_ops(Lam*, Lam*) override;
    const Def* rewrite_imm_Nom(const Nom*) override;
    const Def* rewrite_imm_Name(const Name*) override;
    const Def* rewrite_imm_Struc(const Struc*) override;
    const Def* rewrite_imm_App(const App*) override;
    const Def* buf_of(const Def*);

    const Def* lower_from_array(const App*);
    const Def* uncurry(Lam*);
    const Def* lower_get(const App*);
    const Def* lower_set(const App*);
    const Def* lower_splat(const App*);
    const Def* lower_generate(const App*);
    const Def* lower_broadcast(const App*);
    const Def* lower_map_reduce(const App*);
    const Def* lower_pad(const App*);
    const Def* lower_concat(const App*);
    const Def* lower_gather(const App*);
    const Def* lower_scatter(const App*);
    const Def* splat_buffer(const Def*, const Def*);

    /// A `⊥: mem.M 0` placeholder consumed by emitted buffer operations; AddMem replaces it with the
    /// scheduler-placed current memory.
    /// Shared by every op whose result is a pure function of its value operands, so that genuinely equal
    /// ops still collapse into one (e.g. a weight literal materialized at many sites).
    const Def* bot_mem();

    /// A *fresh* `mem.M 0` for one emitted buffer/btensor operation: the result of a lifted `mem.fresh 0`.
    /// Required by every op that allocates a buffer it then writes into, for two independent reasons:
    /// 1. Immutable Def%s are hash-consed, so two `buffer.alloc`s agreeing on `(r, s, T)` and sharing one
    ///    placeholder collapse into a single allocation - two distinct tensors would alias one buffer.
    /// 2. AddMem is a memoizing Rewriter, so two operations sharing one argument tuple
    ///    `(⊥: mem.M 0, …)` - which happens whenever they differ only in their curried callee - are threaded
    ///    from the *same* current memory, and the resulting parallel mem chains collapse in `cps.conv`.
    /// The continuation has filter `tt`, so it vanishes as soon as AddMem substitutes the real memory.
    const Def* fresh_mem();

    flags_t tensor_key_ = 0;
    LamSet shape_poly_;
    bool tensors_seen_ = false;
    /// Per-lam memo for the ops that consume a fresh memory (see LowerToMem::rewrite).
    DefMap<const Def*> fresh_memo_;
};

} // namespace mim::plug::tensor::phase
