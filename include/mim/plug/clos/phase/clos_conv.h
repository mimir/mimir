#pragma once

#include <queue>

#include <mim/phase.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

/// Computes, on demand and with memoization, the free Def%s that a Lam must capture in its closure environment.
///
/// A referenced nested mutable does not become a free def itself; instead it contributes *its* free defs, since it
/// will be closure-converted too.
/// This makes the free-def sets of (mutually) recursive Lam%s inter-dependent, so they are solved to a fixed point
/// over a little dependency graph: an edge `pred → node` means `node` must include all of `pred`'s free defs.
///
/// Mutables are only treated as free defs directly if annotated with clos::attr::free_bb / clos::attr::fstclass_bb;
/// immutables carrying free vars are broken down to their relevant leaves.
class FreeDefAna {
public:
    FreeDefAna(World& world)
        : world_(world) {}

    /// Returns the free defs @p lam has to capture; see the class description.
    const DefSet& run(Lam* lam);

private:
    /// A memoized free-def set together with its dependency edges in the fixed-point graph.
    struct Node {
        Def* mut;
        DefSet fvs;
        Vector<Node*> preds; ///< Nodes whose free defs must flow into this one.
        Vector<Node*> succs; ///< Reverse of preds; used to re-enqueue dependents when fvs grow.
        unsigned pass = 0;   ///< 0 = uninitialized; otherwise the pass that last touched this node.

        auto add_fvs(const Def* def) {
            assert(!Axm::isa<mem::M>(def->type()));
            return fvs.emplace(def);
        }
    };

    using NodeQueue = std::queue<Node*>;

    bool is_bot(Node* node) const { return node->pass == 0; }                          ///< Not yet initialized?
    bool is_done(Node* node) const { return !is_bot(node) && node->pass < cur_pass_; } ///< Settled in an earlier pass?
    void mark(Node* node) { node->pass = cur_pass_; }

    /// Classifies a free def of @p node, either adding it as a captured fv or spawning/linking a predecessor Node.
    /// @p spawned_pred is set if this created a fresh (uninitialized) predecessor Node.
    void classify(Node* node, const Def* fd, bool& spawned_pred, NodeQueue& worklist);

    std::pair<Node*, bool> build_node(Def* mut, NodeQueue& worklist);
    void propagate(NodeQueue& worklist);

    World& world() { return world_; }

    World& world_;
    unsigned cur_pass_ = 1;
    DefMap<std::unique_ptr<Node>> lam2node_;
};

/// Performs *typed closure conversion*, rebuilding the old world into a new one.
/// This is based on the [Simply Typed Closure Conversion](https://dl.acm.org/doi/abs/10.1145/237721.237791).
/// Closures are represented using tuples: `[Env: *, Cn [Env, Args..], Env]`.
/// In general only *continuations* are converted; different kinds of Lam%s are treated differently:
/// - *returning continuations* ("functions"), *join-points* and *branches* are fully closure converted.
/// - *return continuations* are not closure converted.
/// - *first-class continuations* get a "dummy" closure; they still have free variables.
///
/// This phase relies on ClosConvPrep to introduce annotations for these cases.
///
/// A converted Lam is first stubbed (ClosConv::make_stub) and packed at each use site, while its body is
/// enqueued and rewritten later (ClosConv::rewrite_body) in its own fresh substitution scope.
/// This isolation is essential: inside a closure's body its free defs are replaced by projections of *its own*
/// environment, and those substitutions must not leak into the enclosing scope where the closure was packed.
///
/// @note Direct-style Def%s are not rewritten, which can be a problem for certain Axm%s such as
/// `ax : (B : *, int → B) → (int → B)`.
/// There is also no machinery for free variables in a Lam's type, which may break polymorphic functions.
class ClosConv : public RWPhase {
public:
    ClosConv(World& world, flags_t annex)
        : RWPhase(world, annex)
        , fva_(world) {} // the FVA operates on the old world

private:
    /// A closure-converted Lam: the code part @p fn capturing the free defs @p fvs of @p old_fn.
    struct Stub {
        Lam* old_fn;
        /// The captured free defs.
        /// Do not recover them from the env tuple's ops: World::tuple may normalize it (η-reduction back to the
        /// underlying def, unary tuple, uniform Pack).
        DefVec fvs;
        Lam* fn;
    };

    void start() override;

    /// @name Rewrite hooks
    ///@{
    const Def* rewrite_imm_Pi(const Pi*) final;
    const Def* rewrite_mut_Pi(Pi*) final;
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* rewrite_mut_Global(Global*) final;
    ///@}

    /// Handles the `%clos.attr.{returning,free_bb,fstclass_bb}` wrappers; returns `nullptr` if @p a is none of these.
    const Def* rewrite_attr(Axm::IsA<attr, App> a);

    Stub make_stub(const DefSet& fvs, Lam* old_lam);
    Stub make_stub(Lam* old_lam);
    void rewrite_body(const Stub&);

    /// Builds the closure type for the `Cn` @p pi; with @p env_type, the bare code `Cn` instead of the Sigma.
    const Def* clos_type_of(const Pi* pi, const Def* env_type = nullptr);
    /// Rewrites a return continuation's type: stays a plain `Cn`, but its domains are closure-converted.
    const Pi* rewrite_ret_cn(const Pi*);

    FreeDefAna fva_;
    DefMap<Stub> closures_; ///< old_fn *and* new fn ↦ Stub.

    /// Muts that must be rewritten uniformly across the whole module: closure types and globals.
    /// Such muts must not depend on defs living inside the scope of a continuation.
    Def2Def glob_muts_;

    std::queue<Lam*> body_worklist_; ///< New fns whose bodies are yet to be rewritten.
    bool converting_ = false;        ///< `false` while bootstrapping annexes; `true` once actually converting.
};

} // namespace mim::plug::clos::phase
