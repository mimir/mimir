#pragma once

#include <memory>

#include "mim/def.h"
#include "mim/lam.h"
#include "mim/nest.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
namespace mim::plug::sflow {

class Tab;

class CFG {
public:
    class Loop;

    class Node {
    public:
        const Lam* mut() const { return mut_; }
        const auto& succs() const { return succs_; }
        const auto& preds() const { return preds_; }
        /// [Immediate Dominator](https://en.wikipedia.org/wiki/Dominator_(graph_theory)) of this Node, or itself
        /// if it is the entry. Computed eagerly when the CFG is constructed.
        const Node* idom() const { return idom_; }
        /// Nodes higher up in the dominator tree have higher postorder numbers.
        /// Used to efficiently find the LCA in the dominator tree via CFG::lca.
        size_t postorder_number() const { return postorder_number_; }
        /// Deepest Loop containing this Node, or `nullptr` if not in any loop.
        /// Triggers lazy computation of the loop hierarchy as needed: forces
        /// top-level loops to be computed first, then walks down the chain of
        /// containing loops, expanding each level until no deeper loop is
        /// found for this Node.
        const Loop* loop() const;

    private:
        Node(CFG& cfg, const Lam* mut)
            : cfg_(cfg) {
            mut_ = mut;
        }

        void follow_escaping(const App* app);
        bool add_edge(const Lam* succ);
        void init();

        CFG& cfg_;
        const Lam* mut_;
        const Node* idom_        = nullptr;
        size_t postorder_number_ = 0;

        absl::flat_hash_set<Node*> succs_;
        absl::flat_hash_set<Node*> preds_;
        const Loop* loop_ = nullptr;

        // Scratch state for compute_sccs (Tarjan's algorithm).
        static constexpr uint32_t Unvisited = uint32_t(-1);
        uint32_t idx_                       = Unvisited;
        uint32_t low_                       = 0;
        bool on_stack_                      = false;

        friend class CFG;
        friend class Loop;
    };

    using SCC  = absl::flat_hash_set<const Node*>;
    using SCCs = std::vector<std::unique_ptr<SCC>>;

    class Loop {
    public:
        const Loop* parent() const { return parent_; }
        /// Immediate child loops. Computed lazily on first access.
        const auto& children() const {
            calc_nested_loops();
            return children_;
        }
        const auto& nodes() const { return nodes_; }
        const auto& entries() const { return entries_; }
        const auto& exits() const { return exits_; }

    private:
        Loop(const Loop* parent)
            : parent_(parent) {}

        void calc_nested_loops() const {
            if (!nested_computed_) {
                nested_computed_ = true;
                find_nested_loops();
            }
        }
        /// Find immediate nested loops within this loop by recomputing SCCs
        /// on this loop's nodes with edges into entries() blocked, breaking
        /// back edges to expose the next level of nesting. Does not recurse;
        /// deeper levels are computed lazily when their children() is accessed.
        void find_nested_loops() const;

        const Loop* parent_;
        mutable std::vector<std::unique_ptr<Loop>> children_;
        mutable bool nested_computed_ = false;
        absl::flat_hash_set<const Node*> nodes_;
        absl::flat_hash_set<const Node*> entries_;
        absl::flat_hash_set<const Node*> exits_;

        friend class CFG;
    };

    /// Construct a CFG rooted at the Lam represented by @p entry.
    /// @p entry must be a Nest::Node whose `mut()` is a Lam. The CFG includes
    /// this Lam and every Lam whose Nest::Node is a descendant of @p entry in
    /// the nesting tree.
    CFG(const Nest::Node* entry);

    void cfa();

    const Node* entry() const { return entry_; }
    const Nest::Node* nest_entry() const { return nest_entry_; }

    /// @name Dominance
    ///@{
    /// Least common ancestor of @p n and @p m in the dominator tree.
    static const Node* lca(const Node* n, const Node* m);
    ///@}

    /// @name SCCs / Loops
    ///@{
    /// Computes SCCs of @p nodes using Tarjan's algorithm.
    /// Edges to nodes outside @p nodes are ignored.
    /// Edges into nodes in @p blocked are ignored as well — useful for breaking back edges.
    SCCs compute_sccs(const absl::flat_hash_set<const Node*>& nodes,
                      const absl::flat_hash_set<const Node*>& blocked = {}) const;

    /// Top-level loops in this CFG. Each loop may contain nested loops via Loop::children().
    /// Computed lazily on first access.
    const auto& loops() const {
        calc_loops();
        return loops_;
    }
    ///@}

    /// @name Nodes
    ///@{
    size_t num_nodes() const { return mut2node_.size(); }
    // clang-format off
    auto muts()  const { return mut2node_ | std::views::keys; }
    auto nodes() const { return mut2node_ | std::views::transform([](const auto& p) { return (const Node*)p.second.get(); }); }
    // clang-format on
    const Node* operator[](const Def* def) const { return (def->isa<Lam>() ? (*this)[def->as<Lam>()] : nullptr); }
    ///@}

    /// @name dot
    /// GraphViz output.
    ///@{
    void dot(std::ostream& os) const;
    void dot(const char* file = nullptr) const;
    void dot(std::string s) const { dot(s.c_str()); }
    /// Emits this CFG as a labelled `subgraph cluster_*` block plus its edges.
    /// Shares @p cluster_id with the caller so sibling clusters don't collide.
    void dot_cluster(std::ostream& os, mim::Tab& tab, size_t& cluster_id) const;
    ///@}

private:
    Node* visit(const Lam* mut);

    void assign_postorder_numbers();
    void calc_dominance();
    void calc_loops() const {
        if (!loops_computed_) {
            loops_computed_ = true;
            find_loops();
        }
    }
    void find_loops() const;
    static std::unique_ptr<Loop> make_loop(const SCC& scc, const Loop* parent);

    Node* operator[](const Lam* mut) {
        if (auto i = mut2node_.find(mut); i != mut2node_.end()) return i->second.get();
        return nullptr;
    }

    World& world_;
    const Nest::Node* nest_entry_;
    absl::flat_hash_map<const Lam*, std::unique_ptr<Node>> mut2node_;
    Node* entry_;
    mutable std::vector<std::unique_ptr<Loop>> loops_;
    mutable bool loops_computed_ = false;
};

/// @name Nest CFG helpers
///@{
std::unique_ptr<CFG> nest_cfg(const Nest::Node* node);
std::unique_ptr<CFG> nest_cfg(const Nest& nest);
void nest_cfg_dot(const Nest& nest, std::ostream& os);
void nest_cfg_dot(const Nest& nest, const char* file = nullptr);
///@}

} // namespace mim::plug::sflow
