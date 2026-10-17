#pragma once

#include <mim/nest.h>

#include "mim/plug/mem/escape_analysis.h"

namespace mim::plug::mem::phase {

/// Frees the memory a function allocates but does not let escape; see mem::EscapeAnalysis.
///
/// An allocation only its home's own memory chain mentions is freed right after its last use there.
/// Any other is freed as control passes from where it is live to where it is dead.
/// It is live on entry to a Nest node if the node or a sibling it or one of its ancestors reaches mentions it.
/// Since a recursive node reaches itself, this covers loops, too.
/// This is closed under control flow: once dead, an allocation stays dead.
class FreeLocals : public RWPhase {
public:
    FreeLocals(World& world, flags_t annex)
        : RWPhase(world, annex, &escape_)
        , escape_(world) {}

private:
    bool analyze() final;
    const Def* rewrite_imm_App(const App*) final;

    /// Collects the mentions of every node's subtree.
    const DefSet& mentions(const Nest::Node*);
    /// The mentions of every sibling @p node transitively reaches.
    const DefSet& reach(const Nest::Node* node);
    /// Places the frees of @p alloc in @p node%'s subtree, which hangs below its @p home.
    void place(const Nest::Node* home, const Nest::Node* node, const Def* alloc);
    bool live(const Nest::Node* home, const Nest::Node* node, const Def* alloc);
    bool dead(const Nest::Node* home, const Def* target, const Def* alloc);

    /// Rebuilds @p arg with `mem.free`s of the (old-world) @p allocs threaded through its memory @p mem.
    const Def* thread(const Def* arg, const Def* mem, const DefVec& allocs);
    /// Rebuilds a control position of a tail, freeing on the way to the targets in @p edges.
    const Def* rewrite_target(const Def* target, const DefMap<DefVec>& edges);
    /// A continuation that frees @p allocs and jumps on to @p cont.
    const Def* trampoline(const Def* cont, const DefVec& allocs);
    const Def* free(const Def* mem, const DefVec& allocs);

    EscapeAnalysis escape_;
    GIDMap<const Nest::Node*, DefSet> mentions_;
    GIDMap<const Nest::Node*, DefSet> reach_;
    /// Old op or tail ↦ allocations to free right before it.
    DefMap<DefVec> frees_;
    /// Old tail ↦ old target ↦ allocations to free on the way there.
    DefMap<DefMap<DefVec>> edges_;
};

} // namespace mim::plug::mem::phase
