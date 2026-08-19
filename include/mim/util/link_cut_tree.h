#pragma once

#include <cassert>
#include <cstddef>

#include <array>

namespace mim::lct {

/// This is an **intrusive** [Link-Cut-Tree](https://en.wikipedia.org/wiki/Link/cut_tree).
/// Intrusive means that you have to inherit from this class via
/// [CRTP](https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern) like this:
/// ```
/// class Node : public lct::Node<Node, MyKey> {
///     constexpr bool lt(const MyKey& key) const noexcept { ... }
///     constexpr bool eq(const MyKey& key) const noexcept { ... }
///     // ...
/// };
/// ```
template<class P, class K>
class Node {
public:
    /// Index into Node::kids.
    enum Dir : size_t {
        Bot, ///< left/deeper/bottom/leaf-direction
        Top, ///< right/shallower/top/root-direction
    };

private:
    P* self() { return static_cast<P*>(this); }
    const P* self() const { return static_cast<const P*>(this); }

public:
    constexpr Node() noexcept = default;

    ///@name Getters
    ///@{

    /// Find @p k or the element just greater than @p k.
    constexpr P* find(const K& k) noexcept {
        expose();
        auto prev = this;
        for (auto n = this; n;) {
            if (n->self()->eq(k)) return n->splay(), n->self();

            if (n->self()->lt(k)) {
                n = n->kids[Bot];
            } else {
                prev = n;
                n    = n->kids[Top];
            }
        }

        return prev->self();
    }

    [[nodiscard]] bool contains(const K& k) noexcept { return find(k)->eq(k); }
    ///@}

    ///@name parent
    ///@{
    /// Is `this` a child of Node::parent within the same splay tree?
    constexpr bool is_aux_child() const noexcept {
        return parent && (parent->kids[Bot] == this || parent->kids[Top] == this);
    }
    // clang-format off
    constexpr Node* aux_parent() noexcept { return  is_aux_child()           ? parent         : nullptr; }
    constexpr P*   path_parent() noexcept { return !is_aux_child() && parent ? parent->self() : nullptr; }
    // clang-format on
    ///@}

    ///@name Splay Tree
    ///@{

    /// Which of Node::kids is @p kid?
    constexpr Dir dir(const Node* kid) const noexcept {
        assert(kids[Bot] == kid || kids[Top] == kid);
        return kids[Top] == kid ? Top : Bot;
    }

    /// [Splays](https://hackmd.io/@CharlieChuang/By-UlEPFS#Operation1) `this` to the root of its splay tree.
    constexpr void splay() noexcept {
        while (auto p = aux_parent()) {
            auto i = p->dir(this);
            if (auto pp = p->aux_parent()) {
                auto j = pp->dir(p);
                if (i == j) // zig-zig/zag-zag
                    pp->rotate(j), p->rotate(i);
                else // zig-zag/zag-zig
                    p->rotate(i), pp->rotate(j);
            } else { // zig/zag
                p->rotate(i);
            }
        }
    }

    /// Helper for Splay-Tree: rotates `this`'s @p i%th kid `c` into `this`'s (`x`'s) place:
    /// ```
    ///  | i == Top              | i == Bot               |
    ///  |-----------------------|------------------------|
    ///  |   p              p    |       p          p     |
    ///  |   |              |    |       |          |     |
    ///  |   x              c    |       x          c     |
    ///  |  / \     ->     / \   |      / \   ->   / \    |
    ///  | a   c          x   d  |     c   a      d   x   |
    ///  |    / \        / \     |    / \            / \  |
    ///  |   b   d      a   b    |   d   b          b   a |
    ///  ```
    constexpr void rotate(Dir i) noexcept {
        auto j = i == Bot ? Top : Bot;
        auto x = this;
        auto p = x->parent;
        auto c = x->kids[i];
        auto b = c->kids[j];

        if (b) b->parent = x;

        // if p is only a path parent, it has no kid to fix up
        if (p) {
            if (p->kids[Bot] == x)
                p->kids[Bot] = c;
            else if (p->kids[Top] == x)
                p->kids[Top] = c;
        }

        x->parent  = c;
        c->parent  = p;
        x->kids[i] = b;
        c->kids[j] = x;
    }
    ///@}

    /// @name Link-Cut-Tree
    ///@{

    /// Registers the edge `this -> child` in the *aux* tree.
    constexpr void link(Node* child) noexcept {
        this->expose();
        child->expose();
        if (!child->kids[Top]) {
            this->parent     = child;
            child->kids[Top] = this;
        }
    }

    /// Make a preferred path from `this` to root while putting `this` at the root of the *aux* tree.
    /// @returns the last valid path_parent().
    constexpr P* expose() noexcept {
        Node* prev = nullptr;
        for (auto curr = this; curr; prev = curr, curr = curr->parent) {
            curr->splay();
            assert(!prev || prev->parent == curr);
            curr->kids[Bot] = prev;
        }
        splay();
        return prev->self();
    }

    /// Least Common Ancestor of `this` and @p other in the *aux* tree; leaves @p other expose%d.
    /// @returns `nullptr`, if @p a and @p b are in different trees.
    constexpr P* lca(Node* other) noexcept { return this->expose(), other->expose(); }

    /// Is `this` a descendant of `other` in the *aux* tree?
    /// Also `true`, if `this == other`.
    constexpr bool is_descendant_of(Node* other) noexcept {
        if (this == other) return true;
        this->expose();
        other->splay();
        auto curr = this;
        while (auto p = curr->aux_parent())
            curr = p;
        return curr == other;
    }
    ///@}

    Node* parent              = nullptr; ///< parent or path-parent
    std::array<Node*, 2> kids = {};      ///< Node::Bot/Node::Top children
};

} // namespace mim::lct
