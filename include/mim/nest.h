#pragma once

#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <stack>

#include "mim/def.h"

namespace mim {

namespace detail {
/// Like GIDLt, but the *nesting virtual root* is represented as a `nullptr` Def%, which must sort first.
struct NullSafeDefGIDLt {
    constexpr bool operator()(const Def* a, const Def* b) const noexcept {
        return (a ? a->gid() : 0) < (b ? b->gid() : 0);
    }
};
} // namespace detail

/// Builds a nesting tree for all mutables/binders.
/// The constructor also computes sibling dependencies, SCCs, loop depths, and dominance.
/// A Nest is immutable afterwards.
class Nest {
public:
    class Node {
    public:
        /// @name Getters
        ///@{
        std::string name() const { return mut() ? mut()->unique_name() : std::string("virtual"); }
        const Nest& nest() const { return nest_; }
        const Node* inest() const { return inest_; } ///< Immediate nester/parent of this Node.
        /// [Immediate Dominator](https://en.wikipedia.org/wiki/Dominator_(graph_theory)) for children in connected
        /// components; used for late code placement in [Nest::lca].
        const Node* idom() const { return idom_; }
        bool is_root() const { return inest_ == nullptr; }
        /// The *mutable* capsulated in this Node or `nullptr`, if it's a *virtual root* comprising several Node%s.
        Def* mut() const {
            assert(mut_ || is_root());
            return mut_;
        }
        uint32_t level() const { return level_; }
        uint32_t loop_depth() const { return loop_depth_; }
        /// Stable id for deterministic hashing/ordering: the underlying mut's gid, or `0` for the (unique) virtual
        /// root.
        uint32_t gid() const { return mut_ ? mut_->gid() : 0; }
        ///@}

        /// @name Children
        ///@{
        struct Children {
            ///@name Get children as muts and/or nodes.
            ///@{
            // clang-format off
            auto muts()     const { return mut2node_ | std::views::keys; }
            auto nodes()    const { return mut2node_ | std::views::transform([](auto p) { return const_cast<const Node*>(p.second); }); }
            // clang-format on
            size_t num() const { return mut2node_.size(); } ///< Number of children.
            ///@}

            /// @name Lookup
            ///@{
            const Node* operator[](Def* mut) const { return fe::lookup(mut2node_, mut); }
            bool contains(Def* mut) const { return mut2node_.contains(mut); } ///< is @p mut a child?
            ///@}

            /// @name Iterators
            ///@{
            auto begin() const { return mut2node_.cbegin(); }
            auto end() const { return mut2node_.cend(); }
            ///@}

        private:
            auto mut_nodes() { return mut2node_ | std::views::values; }

            MutMap<Node*> mut2node_;

            friend class Nest;
        };

        const Children& children() const { return children_; }
        ///@}

        struct SiblDeps {
            /// @name Get sibling dependencies
            ///@{
            auto nodes() const {
                return nodes_ | std::views::transform([](Node* n) { return const_cast<const Node*>(n); });
            }
            ///@}

            /// @name Getters
            ///@{
            size_t num() const { return nodes_.size(); }
            bool contains(const Node* n) const { return nodes_.contains(const_cast<Node*>(n)); }
            ///@}

            /// @name Iterators
            ///@{
            auto begin() const { return nodes_.cbegin(); }
            auto end() const { return nodes_.cend(); }
            ///@}

        private:
            GIDSet<Node*> nodes_;

            friend class Nest;
        };

        /// @name Sibling Dependencies
        /// These are the dependencies across children():
        /// A child `n` depends() on `m`, if a subtree of `n` uses `m`.
        ///@{
        template<bool Forward = true>
        const SiblDeps& sibl_deps() const {
            if constexpr (Forward)
                return sibl_deps_;
            else
                return sibl_rev_deps_;
        }
        ///@}

        /// Strongly Connected Component.
        using SCC = GIDSet<const Node*>;
        /// @name SCCs
        /// [SCCs](https://en.wikipedia.org/wiki/Strongly_connected_component) for all children dependencies.
        /// @note The Nest::root() cannot be is_mutually_recursive() by definition.
        /// If you have a set of mutually recursive Def%s as "root", include them all by using a *virtual root*.
        ///@{
        bool is_recursive() const { return recursive_; }
        bool is_mutually_recursive() const { return is_recursive() && !is_root() && scc().size() > 1; }
        bool is_directly_recursive() const { return is_recursive() && (is_root() || scc().size() == 1); }
        /// The SCC among its siblings this Node belongs to; the Nest::root() has none.
        const SCC& scc() const {
            assert(!is_root());
            return *scc_;
        }
        ///@}

    private:
        Node(const Nest& nest, Def* mut, Node* inest)
            : nest_(nest)
            , mut_(mut)
            , inest_(inest)
            , level_(inest ? inest->level() + 1 : 0) {
            if (inest) inest->children_.mut2node_.emplace(mut, this);
        }

        void link(Node* other) { this->sibl_deps_.nodes_.emplace(other), other->sibl_rev_deps_.nodes_.emplace(this); }
        void dot(fe::Tab, std::ostream&) const;

        using Stack = std::stack<Node*>;
        uint32_t tarjan(uint32_t, Stack&);

        const Nest& nest_;
        Def* mut_;
        Node* inest_;
        uint32_t level_;
        uint32_t loop_depth_ : 31 = 0;
        bool recursive_      : 1  = false;
        SiblDeps sibl_deps_;
        SiblDeps sibl_rev_deps_;
        Children children_;
        std::deque<std::unique_ptr<SCC>> topo_;
        const SCC* scc_   = nullptr;
        const Node* idom_ = nullptr;
        // Nodes higher up in dominator tree within same sibling layer have higher postorder numbers.
        // This property is used to efficiently find the correct node for late code placement via [Nest::lca].
        std::optional<size_t> postorder_number_ = std::nullopt;

        // Tarjan's bookkeeping.
        static constexpr uint32_t Unvisited = uint32_t(-1);
        uint32_t idx_                       = Unvisited;
        uint32_t low_  : 31                 = 0;
        bool on_stack_ : 1                  = false;

        friend class Nest;
    };

    /// @name Constructors
    ///@{
    Nest(Def* root);
    /// *Virtual root* with @p muts as children; @p transitive also adds the closed mutables they reach.
    Nest(World&, fe::View<Def*> muts, bool transitive = false);
    Nest(World&); ///< *Virtual root* with all World::externals as children.
    Nest(const Nest&)     = delete;
    Nest(Nest&&)          = delete;
    Nest& operator=(Nest) = delete;
    ///@}

    /// @name Getters
    ///@{
    World& world() const { return world_; }
    const Node* root() const { return root_; }
    Vars vars() const { return vars_; } ///< All Var%s occurring in this Nest.
    bool contains(const Def* def) const { return def->has_free_vars_in(vars()); }
    bool is_recursive() const { return root_->is_recursive(); }
    ///@}

    /// @name Nodes
    ///@{
    // clang-format off
    auto muts()  const { return mut2node_ | std::views::keys; }
    auto nodes() const { return mut2node_ | std::views::transform([](const auto& p) { return (const Node*)p.second.get(); }); }
    // clang-format on

    const Node* operator[](Def* mut) const { return node(mut); }
    /// The innermost Node binding a free Var of @p def, or the root() if there is none.
    /// For a mutable in this Nest, that is its Node::inest.
    const Node* inest(const Def* def) const { return innermost(def); }
    ///@}

    /// @name Iterators
    ///@{
    auto begin() const { return mut2node_.cbegin(); }
    auto end() const { return mut2node_.cend(); }
    ///@}

    static const Node* lca(const Node* n, const Node* m); ///< Least common ancestor of @p n and @p m.

    /// @name dot
    /// GraphViz output.
    ///@{
    void dot(std::ostream& os, DotConfig cfg = {}) const;
    void dot(const char* file = nullptr, DotConfig cfg = {}) const;
    void dot(std::string s, DotConfig cfg = {}) const { dot(s.c_str(), cfg); }
    ///@}

private:
    Node* node(Def* mut) const {
        if (auto i = mut2node_.find(mut); i != mut2node_.end()) return i->second.get();
        return nullptr;
    }

    Node* innermost(const Def*) const;
    Node* innermost(Node*, Vars) const;
    void analyze();
    void populate(bool transitive);
    Node* make_node(Def*, Node* inest = nullptr);
    void calc_sibl_deps(Node*, fe::Vector<Node*>& path);
    void calc_SCCs(Node*);
    void number(Node*, size_t&);
    void calc_dominance(Node*);
    void sibl_post_order(Node*, ankerl::unordered_dense::set<const Node*>&, fe::Vector<Node*>&);

    World& world_;
    std::map<Def*, std::unique_ptr<Node>, detail::NullSafeDefGIDLt> mut2node_;
    Vars vars_;
    Node* root_;
};

} // namespace mim
