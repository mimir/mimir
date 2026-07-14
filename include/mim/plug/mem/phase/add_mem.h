#pragma once

#include <mim/phase.h>
#include <mim/schedule.h>

namespace mim::plug::mem::phase {

/// Threads the `%mem.M` memory monad through the world:
/// mem-extends continuations and rewires every memory operand to the scheduler-placed current memory.
/// It's primarily to be used as preparation for other phases that rely on all continuations having a mem.
/// It also splices the `⊥ : %mem.M 0` memory placeholders of freshly emitted memory operations into the
/// global memory chain (see mim::plug::tensor::phase::LowerToMem, which runs this phase embedded).
///
/// Three rules keep the rewrite type-correct in the presence of axiom-pinned ABIs:
/// - Only continuations (Pi::isa_cn) are mem-extended; direct-style functions (e.g. the affine index
///   mappings passed to `%matrix.map_reduce_aff`) keep their signature.
/// - A pi whose leading parameter carries the memory *grouped* (the `Fn [%mem.M 0, To, ins] → …` shape of
///   a mem-threaded combiner) counts as already mem-threaded.
/// - Lams reachable from axm-app arguments are preserved untouched: axioms pin their arguments' ABI
///   (e.g. the combiner slot of `%matrix.map_reduce_aff`), so mem-extending them would be ill-typed.
class AddMem : public NestPhase<Lam> {
public:
    AddMem(World& world, flags_t annex)
        : NestPhase(world, annex, true) {}

    void start() override;
    void visit(const Nest&) override;

private:
    const Def* add_mem_to_lams(Lam*, const Def*);
    const Def* rewrite_type(const Def*);
    const Def* rewrite_pi(const Pi*);
    /// Return the most recent memory for the given lambda.
    const Def* mem_for_lam(Lam*) const;
    /// Record the most recent memory of `place` — also under its rewritten lam, where `mem_for_lam` looks
    /// (the scheduler yields old-world lams; a mem-extended lam is a different mutable).
    void set_mem(Lam* place, const Def* mem);

    Scheduler sched_;
    /// Stores the most recent memory for a lambda.
    Def2Def val2mem_;
    /// Memoization & Association for rewritten defs.
    Def2Def mem_rewritten_;
    /// Lams (transitively) reachable from axm-app arguments: their ABI is pinned by the axiom.
    LamSet preserved_;
};

} // namespace mim::plug::mem::phase
