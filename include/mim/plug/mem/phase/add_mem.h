#pragma once

#include <mim/phase.h>

namespace mim::plug::mem::phase {

/// Threads the `%mem.M` memory monad through the world:
/// mem-extends continuations and rewires every memory operand to the *current* memory at that program point.
/// It's primarily to be used as preparation for other phases that rely on all continuations having a mem.
/// It also splices the `⊥ : %mem.M 0` memory placeholders of freshly emitted memory operations into the
/// global memory chain (see mim::plug::tensor::phase::LowerToMem, which runs this phase embedded).
///
/// The rewrite is a plain RWPhase.
/// Memory is a linear resource, so at any program point exactly one memory token is live - the *current* memory.
/// We track it in AddMem::curr_mem_ while rewriting a continuation's body:
/// - it starts out as the continuation's (leading or grouped) mem parameter,
/// - a rewritten memory operation advances it to the operation's result mem, and
/// - every memory-typed *operand* is rewritten to the current memory.
///
/// Because the Rewriter visits operands before their users, the current memory naturally threads through the
/// data-dependency order - no separate schedule is required.
/// Independent `⊥`/`⊤` placeholder chains are thereby linearized into one chain in encounter order.
///
/// Three rules keep the rewrite type-correct in the presence of axiom-pinned ABIs:
/// - Only continuations (Pi::isa_cn) are mem-extended; direct-style functions (e.g. the affine index
///   mappings passed to `%matrix.map_reduce_aff`) keep their signature.
/// - A pi whose leading parameter carries the memory *grouped* (the `Fn [%mem.M 0, To, ins] → …` shape of
///   a mem-threaded combiner) counts as already mem-threaded.
/// - Lams reachable from axm-app arguments are preserved untouched: axioms pin their arguments' ABI
///   (e.g. the combiner slot of `%matrix.map_reduce_aff`), so mem-extending them would be ill-typed.
class AddMem : public RWPhase {
public:
    AddMem(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void start() override;
    const Def* rewrite(const Def*) override;
    const Def* rewrite_mut_Lam(Lam*) override;
    const Def* rewrite_imm_App(const App*) override;
    const Def* rewrite_imm_Tuple(const Tuple*) override;
    const Def* rewrite_imm_Pi(const Pi*) override;

    /// Advances AddMem::curr_mem_ if @p def produces a memory (a bare `%mem.M` or a `[%mem.M, …]` tuple).
    void advance_mem(const Def* def);

    /// The current memory token in the continuation being rewritten (new world), or `nullptr` outside any mem context.
    const Def* curr_mem_ = nullptr;
    /// `true` while rewriting a pinned-ABI subtree (an axm-app argument): no memory is threaded or added there.
    bool preserving_ = false;
    /// Lams (transitively) reachable from axm-app arguments: their ABI is pinned by the axiom.
    LamSet preserved_;
};

} // namespace mim::plug::mem::phase
