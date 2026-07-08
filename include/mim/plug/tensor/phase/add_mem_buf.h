#pragma once

#include <mim/lam.h>
#include <mim/phase.h>
#include <mim/schedule.h>

namespace mim::plug::tensor::phase {

/// A buffer-aware variant of `%mem.add_mem_phase` (mim::plug::mem::phase::AddMem): threads the `%mem.M`
/// memory monad through a freshly bufferized world (see LowerToMem, which runs this phase embedded).
/// Like AddMem it mem-extends continuations and rewires every memory operand to the scheduler-placed
/// current memory — this subsumes what LowerToMem would otherwise have to hand-roll per continuation
/// (return/error continuations, join points, branch arms, interleaving with the caller's own memory ops).
///
/// It deviates from AddMem in three ways, all required by the bufferized world (and unwanted in AddMem's
/// clos-pipeline habitat, hence the separate class):
/// - Only continuations (Pi::isa_cn) are mem-extended; direct-style functions (e.g. the affine index
///   mappings passed to `%matrix.map_reduce_aff`) keep their signature.
/// - A pi whose leading parameter carries the memory *grouped* (the `Fn [%mem.M 0, To, ins] → …` shape of
///   the mem-threaded combiner) counts as already mem-threaded.
/// - Lams reachable from axm-app arguments are preserved untouched: axioms pin their arguments' ABI
///   (e.g. the combiner slot of `%matrix.map_reduce_aff`), so mem-extending them would be ill-typed.
class AddMemBuf : public NestPhase<Lam> {
public:
    AddMemBuf(World& world)
        : NestPhase(world, "add_mem_buf", true) {}

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

} // namespace mim::plug::tensor::phase
