#pragma once

#include <memory>
#include <optional>
#include <ranges>

#include "mim/def.h"

namespace mim {

/// Builds a nesting tree for all mutables/binders.
///
/// @note This type should typically be constructed as `const`,
/// because some member functions used during lookup have private non-`const` overloads.
/// ```
/// const auto nest = Nest(lam);
/// ```
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
        /// components. This is used to transform first order programs into structured form in the
        /// [sflow](mim::plug::sflow) plugin and for late code placement in [Nest::lca].
        auto idom() const { return calc_dominance()->idom_; }
        bool is_root() const { return inest_ == nullptr; }
        /// The *mutable* capsulated in this Node or `nullptr`, if it's a *virtual root* comprising several Node%s.
        Def* mut() const {
            assert(mut_ || is_root());
            return mut_;
        }
        uint32_t level() const { return level_; }
        uint32_t loop_depth() const { return sccs().loop_depth_; }
        ///@}

        /// @name Children
        ///@{
        struct Children {
            ///@name Get children as muts and/or nodes.
            ///@{
            // clang-format off
            auto mut2node() const { return mut2node_ | std::views::transform([](auto p) { return std::pair{p.first, const_cast<const Node*>(p.second)}; }); }
            auto muts()     const { return mut2node_ | std::views::keys; }
            auto nodes()    const { return mut2node_ | std::views::transform([](auto p) { return const_cast<const Node*>(p.second); }); }
            // clang-format on
            size_t num() const { return mut2node_.size(); } ///< Number of children.
            ///@}

            /// @name Lookup
            ///@{
            const Node* operator[](Def* mut) const { return const_cast<Children*>(this)->operator[](mut); }
            bool contains(Def* mut) const { return mut2node_.contains(mut); } ///< is @p mut a child?
            ///@}

            /// @name Iterators
            ///@{
            auto begin() const { return mut2node_.cbegin(); }
            auto end() const { return mut2node_.cend(); }
            ///@}

        private:
            const auto& mut2node() { return mut2node_; }
            auto nodes() { return mut2node_ | std::views::values; }
            auto muts() { return mut2node_ | std::views::keys; }
            auto begin() { return mut2node_.begin(); }
            auto end() { return mut2node_.end(); }
            Node* operator[](Def* mut) { return mim::lookup(mut2node_, mut); }

            MutMap<Node*> mut2node_;

            friend class Nest;
        };

        const Children& children() const { return children_; }
        Children& children() { return children_; }
        ///@}

        template<bool Forward>
        struct SiblDeps {
            /// @name Get sibling dependencies
            ///@{
            auto nodes() const {
                return nodes_ | std::views::transform([](Node* n) { return const_cast<const Node*>(n); });
            }
            ///@}

            /// @name Getters
            ///@{
            size_t num() const { return nodes().size(); }
            bool contains(const Node* n) const { return nodes_.contains(n); }
            ///@}

            /// @name Iterators
            ///@{
            auto begin() const { return nodes_.cbegin(); }
            auto end() const { return nodes_.cend(); }
            ///@}

        private:
            const auto& nodes() { return nodes_; }
            auto begin() { return nodes_.begin(); }
            auto end() { return nodes_.end(); }

            absl::flat_hash_set<Node*> nodes_;

            friend class Nest;
        };

        /// @name Sibling Dependencies
        /// These are the dependencies across children():
        /// A child `n` depends() on `m`, if a subtree of `n` uses `m`.
        ///@{
        template<bool Forward = true>
        auto& sibl_deps() {
            nest().calc_sibl_deps();
            if constexpr (Forward)
                return sibl_deps_;
            else
                return sibl_rev_deps_;
        }

        template<bool Forward = true>
        const auto& sibl_deps() const {
            return const_cast<Node*>(this)->sibl_deps<Forward>();
        }
        ///@}

        /// Strongly Connected Component.
        using SCC = absl::flat_hash_set<const Node*>;
        /// @name SCCs
        /// [SCCs](https://en.wikipedia.org/wiki/Strongly_connected_component) for all children dependencies.
        /// @note The Nest::root() cannot be is_mutually_recursive() by definition.
        /// If you have a set of mutually recursive Def%s as "root", include them all by using a *virtual root*.
        ///@{
        const auto& SCCs() { return sccs().SCCs_; }
        const auto& topo() const { return sccs().topo_; } ///< Topological sorting of all SCCs.
        bool is_recursive() const { return sccs().recursive_; }
        bool is_mutually_recursive() const { return is_recursive() && inest_ && inest_->SCCs_[this]->size() > 1; }
        bool is_directly_recursive() const { return is_recursive() && (!inest_ || inest_->SCCs_[this]->size() == 1); }
        ///@}

    private:
        Node(const Nest& nest, Def* mut, Node* inest)
            : nest_(nest)
            , mut_(mut)
            , inest_(inest)
            , level_(inest ? inest->level() + 1 : 0) {
            if (inest) inest->children_.mut2node_.emplace(mut, this);
        }

        const Node& sccs() const { return nest().calc_SCCs(), *this; }

        void link(Node* other) { this->sibl_deps_.nodes_.emplace(other), other->sibl_rev_deps_.nodes_.emplace(this); }
        void dot(fe::Tab, std::ostream&) const;

        /// SCCs
        using Stack = std::stack<Node*>;
        void calc_SCCs();
        uint32_t tarjan(uint32_t, Node*, Stack&);

        /// Dominance
        const Node* calc_dominance() const;

        const Nest& nest_;
        Def* mut_;
        Node* inest_;
        uint32_t level_;
        uint32_t loop_depth_ : 31 = 0;
        bool recursive_      : 1  = false;
        SiblDeps<true> sibl_deps_;
        SiblDeps<false> sibl_rev_deps_;
        Children children_;
        std::deque<std::unique_ptr<SCC>> topo_;
        absl::flat_hash_map<const Node*, const SCC*> SCCs_;
        mutable const Node* idom_ = nullptr;
        // Nodes higher up in dominator tree within same sibling layer have higher postorder numbers.
        // This property is used to efficiently find the correct node for late code placement via [Nest::lca].
        mutable std::optional<size_t> postorder_number_ = std::nullopt;

        // implementaiton details
        static constexpr uint32_t Unvisited = uint32_t(-1);
        uint32_t idx_                       = Unvisited;
        uint32_t low_  : 31                 = 0;
        bool on_stack_ : 1                  = false;
        Node* curr_child                    = nullptr;

        friend class Nest;
    };

    /// @name Constructors
    ///@{
    Nest(Def* root);
    Nest(View<Def*> muts); ///< Constructs a *virtual root* with @p muts as children.
    Nest(World&);          ///< *Virtual root* with all World::externals as children.
    Nest(const Nest&)     = delete;
    Nest(Nest&&)          = delete;
    Nest& operator=(Nest) = delete;
    ///@}

    /// @name Getters
    ///@{
    World& world() const { return world_; }
    const Node* root() const { return root_; }
    Vars vars() const { return vars_; } ///< All Var%s occurring in this Nest.
    bool contains(const Def* def) const { return vars().has_intersection(def->free_vars()); }
    bool is_recursive() const { return calc_SCCs().root()->is_recursive(); }
    ///@}

    /// @name Nodes
    ///@{
    size_t num_nodes() const { return mut2node_.size(); }
    // clang-format off
    auto muts()  const { return mut2node_ | std::views::keys; }
    auto nodes() const { return mut2node_ | std::views::transform([](const auto& p) { return (const Node*)p.second.get(); }); }
    // clang-format on

    const Node* operator[](Def* mut) const {
        if (auto i = mut2node_.find(mut); i != mut2node_.end()) return i->second.get();
        return nullptr;
    }
    ///@}

    /// @name Iterators
    ///@{
    auto begin() const { return mut2node_.cbegin(); }
    auto end() const { return mut2node_.cend(); }
    ///@}

    template<bool bootstrapping = false>
    static const Node* lca(const Node* n, const Node* m); ///< Least common ancestor of @p n and @p m.

    /// @name dot
    /// GraphViz output.
    ///@{
    void dot(std::ostream& os) const;
    void dot(const char* file = nullptr) const;
    void dot(std::string s) const { dot(s.c_str()); }
    ///@}

private:
    auto begin() { return mut2node_.begin(); }
    auto end() { return mut2node_.end(); }

    void populate();
    Node* make_node(Def*, Node* inest = nullptr);
    void calc_sibl_deps(Node*) const;
    void calc_SCCs(Node*) const;
    void assign_postorder_numbers() const;
    Node* operator[](Def* mut) { return const_cast<Node*>(std::as_const(*this)[mut]); }

    void calc_sibl_deps() const {
        if (!siblings_) {
            siblings_ = true;
            calc_sibl_deps(root_);
        }
    }

    const Nest& calc_SCCs() const {
        if (!sccs_) {
            sccs_ = true;
            calc_SCCs(root_);
        }
        return *this;
    }

    World& world_;
    absl::flat_hash_map<Def*, std::unique_ptr<Node>> mut2node_;
    Vars vars_;
    Node* root_;
    mutable bool siblings_ = false;
    mutable bool sccs_     = false;
};

} // namespace mim
