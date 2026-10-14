#pragma once

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/phase.h>

namespace mim::plug::btensor::phase {

/// Frees the buffers a function allocates but neither returns nor otherwise lets escape, right after their last use.
///
/// Runs on the mem-threaded buffer world (after `mem.add_mem`, before LowerMapReduce), where every allocation is a
/// `buffer.alloc` / `init` / `lit` and the `btensor.*` ops merely write into their `dst` and return it.
/// Per function, a CFG over its nested continuations is built - direct jumps, branches, `affine.For` body/exit,
/// the continuations handed to a callee - and a liveness analysis over the allocations places `buffer.free`s at
/// the death points: after the last use within a continuation's memory chain, or on a CFG edge via a trampoline
/// continuation that frees and jumps on.
///
/// Conservative by construction:
/// - A buffer that flows into *any* tail-call argument - returned, passed to a callee, handed to a join point, carried
///   through a loop - escapes and is never freed; so does everything a closure mentions that leaves the function.
/// - Values of unknown origin (e.g. the result of a direct-style call) count as using every buffer that flowed into
///   them, so aliasing through such ops keeps the source alive.
/// - A continuation whose targets cannot be resolved makes every buffer live at that point.
/// A callee is assumed to invoke each continuation it is handed at most once.
class FreeLocals : public RWPhase {
public:
    FreeLocals(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void start() override;
    const Def* rewrite(const Def*) override;
    const Def* rewrite_imm_App(const App*) override;

    /// Rewrites a control position of a tail call, substituting trampolines for the successors with pending frees.
    const Def* rewrite_control(const Def*, Lam* pred);
    /// A continuation that frees @p allocs and jumps on to @p cont.
    const Def* trampoline(const Def* cont, const DefVec& allocs);
    /// Threads `buffer.free`s of the (old-world) @p allocs through @p mem.
    const Def* free_all(const Def* mem, const DefVec& allocs);

    /// Old memory result ↦ allocations to free right after it.
    DefMap<DefVec> after_;
    /// Old predecessor ↦ old successor ↦ allocations dying on that CFG edge.
    LamMap<LamMap<DefVec>> edges_;
    /// Old node ↦ its tail call.
    LamMap<const App*> tails_;
    /// Old ops whose result tuple a consumer takes as a whole; rebuilt from projections so the memory hook fires.
    DefSet whole_;
};

} // namespace mim::plug::btensor::phase
