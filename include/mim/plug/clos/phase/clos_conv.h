#pragma once

#include <queue>
#include <vector>

#include "mim/phase.h"

#include "mim/plug/clos/clos.h"
#include "mim/plug/mem/mem.h"

namespace mim::plug::clos {

/// Transitively compute free Def's on demand.
/// This is used internally by ClosConv.
class FreeDefAna {
public:
    FreeDefAna(World& world)
        : world_(world)
        , cur_pass_id(1)
        , lam2nodes_() {}

    /// FreeDefAna::run will compute free defs (FD) that appear in @p lam%s body.
    /// Mutable Def%s are only considered free if they are annotated with Clos::freeBB or
    /// Clos::fstclassBB.
    /// Otherwise, we add a mut's free defs in order to build a closure for it.
    /// Immutables containing mutables are broken up if necessary.
    DefSet& run(Lam* lam);

private:
    /// @name analysis graph
    /// @{
    struct Node;
    using NodeQueue = std::queue<Node*>;
    using Nodes     = std::vector<Node*>;

    /// In order to avoid recomputing FDs sets, sets are computed for the subgraph reachable from mut and memorized.
    /// `pass_id` determines if the node has been initialized.
    struct Node {
        Def* mut;
        DefSet fvs;
        Nodes preds;
        Nodes succs;
        unsigned pass_id; //

        auto add_fvs(const Def* def) {
            assert(!Axm::isa<mem::M>(def->type()));
            return fvs.emplace(def);
        }
    };
    /// @}

    /// Node is not initialized?
    bool is_bot(Node* node) { return node->pass_id == 0; }

    /// FD set for node is already present?
    bool is_done(Node* node) { return !is_bot(node) && node->pass_id < cur_pass_id; }

    /// Marks a node as done.
    void mark(Node* node) { node->pass_id = cur_pass_id; }

    /// Split a free Def. This may create more Nodes as more reachable nodes are discovered.
    void split_fd(Node* node, const Def* fv, bool& is_init, NodeQueue& worklist);

    std::pair<Node*, bool> build_node(Def* mut, NodeQueue& worklist);
    void run(NodeQueue& worklist);

    World& world() { return world_; }

    World& world_;
    unsigned cur_pass_id;
    DefMap<std::unique_ptr<Node>> lam2nodes_;
};

/// Performs *typed closure conversion*.
/// This is based on the [Simply Typed Closure Conversion](https://dl.acm.org/doi/abs/10.1145/237721.237791).
/// Closures are represented using tuples: `[Env: *, Cn [Env, Args..], Env]`.
/// In general only *continuations* are converted.
/// Different kind of Lam%s may be rewritten differently:
/// - *returning continuations* ("functions"), *join-points* and *branches* are fully closure converted.
/// - *return continuations* are not closure converted.
/// - *first-class continuations* get a "dummy" closure, they still have free variables.
///
/// This pass relies on ClosConvPrep to introduce annotations for these cases.
///
/// Note: Since direct-style Def%s are not rewritten, this can lead to problems with certain Axm%s:
/// `ax : (B : *, int -> B) -> (int -> B)` won't be converted, possible arguments may.
/// Further, there is no machinery to handle free variables in a Lam%s type; this may lead to
/// problems with polymorphic functions.
class ClosConv : public Phase {
public:
    ClosConv(World& world, flags_t annex)
        : Phase(world, annex)
        , fva_(world) {}

    void start() override;

private:
    /// @name closure stubs
    /// @{
    struct Stub {
        Lam* old_fn;
        /// The free defs packed into env.
        /// Do not recover them from env->ops(): World::tuple may normalize the env tuple
        /// (η-reduction back to the underlying def, unary tuple, uniform Pack).
        DefVec fvs;
        const Def* env;
        Lam* fn;
    };

    Stub make_stub(const DefSet& fvs, Lam* lam, Def2Def& subst);
    Stub make_stub(Lam* lam, Def2Def& subst);
    /// @}

    /// @name Recursively rewrite Def%s.
    /// @{
    void rewrite_body(Lam* lam, Def2Def& subst);
    const Def* rewrite(const Def* old_def, Def2Def& subst);
    Def* rewrite_mut(Def* mut, const Def* new_type, Def2Def& subst);
    const Pi* rewrite_type_cn(const Pi*, Def2Def& subst);
    const Def* type_clos(const Pi* pi, Def2Def& subst, const Def* ent_type = nullptr);
    /// @}

    FreeDefAna fva_;
    DefMap<Stub> closures_;

    // Muts that must be re rewritten uniformly across the whole module:
    // Currently, this includes globals and closure types (for typechecking to go through).
    // Such muts must not depend on defs that live inside the scope of a continuation!
    Def2Def glob_muts_;

    std::queue<const Def*> worklist_;
};

}; // namespace mim::plug::clos
