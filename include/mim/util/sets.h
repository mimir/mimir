#pragma once

#include <algorithm>
#include <array>
#include <fstream>
#include <print>

#include <fe/arena.h>

#include "mim/util/link_cut_tree.h"
#include "mim/util/types.h"
#include "mim/util/util.h"
#include "mim/util/vector.h"

namespace mim {

template<class D, size_t N = 16>
class Sets {
private:
    /// Trie Node.
    class Node : public lct::Node<Node, D*> {
    private:
        using LCT = lct::Node<Node, D*>;

    public:
        constexpr Node(u32 id) noexcept
            : parent(nullptr)
            , def(nullptr)
            , size(0)
            , min(size_t(-1))
            , id(id) {}

        constexpr Node(Node* parent, D* def, u32 id) noexcept
            : parent(parent)
            , def(def)
            , size(parent->size + 1)
            , min(parent->def ? parent->min : def->tid())
            , id(id) {
            parent->link(this);
        }

        constexpr bool lt(D* d) const noexcept { return this->is_root() || this->def->tid() < d->tid(); }
        constexpr bool eq(D* d) const noexcept { return this->def == d; }

        void dot(std::ostream& os) {
            using namespace std::string_literals;

            auto node2str = [](const Node* n) {
                return "n_"s + (n->def ? std::to_string(n->def->tid()) : "root"s) + "_"s + std::to_string(n->id);
            };

            std::print(os, "{} [tooltip=\"gid: {}, min: {}\"];\n", node2str(this), def ? def->gid() : 0, min);

            for (const auto& [_, child] : children)
                std::print(os, "{} -> {}\n", node2str(this), node2str(child.get()));
            for (const auto& [_, child] : children)
                child->dot(os);
        }

        ///@name Getters
        ///@{
        constexpr bool is_root() const noexcept { return def == nullptr; }

        /// All tids on the path from the trie root to `this` live within `[Node::min, Node::def->tid()]`.
        [[nodiscard]] bool contains(D* d) noexcept {
            size_t tid = d->tid(), lo = min, hi = def->tid();
            if (tid == lo || tid == hi) return true;
            return lo < tid && tid < hi && LCT::contains(d);
        }

        using LCT::find;
        ///@}

        Node* const parent;
        D* const def;
        const size_t size;
        const size_t min;
        u32 const id;
        GIDMap<D*, fe::Arena::Ptr<Node>> children;
    };

    struct Data {
        constexpr Data(size_t size) noexcept
            : size(size) {}

        size_t size;
        D* elems[];

        struct Equal {
            constexpr bool operator()(const Data* d1, const Data* d2) const noexcept {
                return d1->size == d2->size && std::equal(d1->begin(), d1->end(), d2->begin());
            }
        };

        /// @name Iterators
        ///@{
        constexpr D** begin() noexcept { return elems; }
        constexpr D** end() noexcept { return elems + size; }
        constexpr D* const* begin() const noexcept { return elems; }
        constexpr D* const* end() const noexcept { return elems + size; }
        ///@}

        template<class H>
        friend constexpr H AbslHashValue(H h, const Data* d) noexcept {
            if (!d) return H::combine(std::move(h), 0);
            return H::combine_contiguous(std::move(h), d->elems, d->size);
        }
    };

public:
    class Set {
    private:
        enum class Tag : uintptr_t { Null, Uniq, Data, Node };

        constexpr Set(const Data* data) noexcept
            : ptr_(uintptr_t(data) | uintptr_t(Tag::Data)) {} ///< Data Set.
        constexpr Set(Node* node) noexcept
            : ptr_(uintptr_t(node) | uintptr_t(Tag::Node)) {} ///< Node set.

    public:
        class iterator {
        private:
            constexpr iterator(D* d) noexcept
                : tag_(Tag::Uniq)
                , ptr_(std::bit_cast<uintptr_t>(d)) {}
            constexpr iterator(D* const* elems) noexcept
                : tag_(Tag::Data)
                , ptr_(std::bit_cast<uintptr_t>(elems)) {}
            constexpr iterator(Node* node) noexcept
                : tag_(Tag::Node)
                , ptr_(std::bit_cast<uintptr_t>(node)) {}

        public:
            /// @name Iterator Properties
            ///@{
            using iterator_category = std::forward_iterator_tag;
            using difference_type   = std::ptrdiff_t;
            using value_type        = D*;
            using pointer           = D* const*;
            using reference         = D* const&;
            ///@}

            /// @name Construction
            ///@{
            constexpr iterator() noexcept = default;
            ///@}

            /// @name Increment
            /// @note These operations only change the *view* of this Set; the Set itself is **not** modified.
            ///@{
            constexpr iterator& operator++() noexcept {
                // clang-format off
                switch (tag_) {
                    case Tag::Uniq: return clear();
                    case Tag::Data: return ptr_ = std::bit_cast<uintptr_t>(std::bit_cast<D* const*>(ptr_) + 1), *this;
                    case Tag::Node: {
                        auto node = std::bit_cast<Node*>(ptr_);
                        node      = node->parent;
                        if (node->is_root())
                            clear();
                        else
                            ptr_ = std::bit_cast<uintptr_t>(node);
                        return *this;
                    }
                    default: fe::unreachable();
                }
                // clang-format on
            }

            constexpr iterator operator++(int) noexcept {
                auto res = *this;
                this->operator++();
                return res;
            }
            ///@}

            /// @name Comparisons
            ///@{
            constexpr bool operator==(iterator other) const noexcept {
                return this->tag_ == other.tag_ && this->ptr_ == other.ptr_;
            }
            ///@}

            /// @name Dereference
            ///@{
            constexpr value_type operator*() const noexcept {
                switch (tag_) {
                    case Tag::Uniq: return std::bit_cast<D*>(ptr_);
                    case Tag::Data: return *std::bit_cast<D* const*>(ptr_);
                    case Tag::Node: return std::bit_cast<Node*>(ptr_)->def;
                    default: fe::unreachable();
                }
            }

            constexpr value_type operator->() const noexcept { return this->operator*(); }
            ///@}

            constexpr iterator& clear() noexcept { return *this = {}; }

        private:
            Tag tag_       = Tag::Null;
            uintptr_t ptr_ = 0;

            friend class Set;
        };

        /// @name Construction
        ///@{
        constexpr Set(const Set&) noexcept = default;
        constexpr Set(Set&&) noexcept      = default;
        constexpr Set() noexcept           = default; ///< Null set
        constexpr Set(D* d) noexcept
            : ptr_(uintptr_t(d) | uintptr_t(Tag::Uniq)) {} ///< Uniq set.

        constexpr Set& operator=(const Set&) noexcept = default;
        ///@}

        /// @name Getters
        ///@{
        constexpr size_t size() const noexcept {
            if (isa_uniq()) return 1;
            if (auto d = isa_data()) return d->size;
            if (auto n = isa_node()) return n->size;
            return 0; // empty
        }

        /// Is empty?
        constexpr bool empty() const noexcept {
            assert(tag() != Tag::Node || !ptr<Node>()->is_root());
            return ptr_ == 0;
        }

        constexpr explicit operator bool() const noexcept { return !empty(); } ///< Not empty?
        ///@}

        /// @name Check Membership
        ///@{

        /// Is @f$d \in this@f$?.
        bool contains(D* d) const noexcept {
            if (auto u = isa_uniq()) return d == u;

            if (auto data = isa_data()) {
                for (auto e : *data)
                    if (d == e) return true;
                return false;
            }

            if (auto n = isa_node()) return n->contains(d);

            return false;
        }

        /// Is @f$this \cap other \neq \emptyset@f$?.
        [[nodiscard]] bool has_intersection(Set other) const noexcept {
            if (this->empty() || other.empty()) return false;
            if (*this == other) return true;

            auto u1 = this->isa_uniq();
            auto u2 = other.isa_uniq();
            if (u1) return other.contains(u1);
            if (u2) return this->contains(u2);

            auto d1 = this->isa_data();
            auto d2 = other.isa_data();
            if (d1 && d2) {
                for (auto ai = d1->begin(), ae = d1->end(), bi = d2->begin(), be = d2->end(); ai != ae && bi != be;) {
                    if (*ai == *bi) return true;

                    if ((*ai)->gid() < (*bi)->gid())
                        ++ai;
                    else
                        ++bi;
                }

                return false;
            }

            auto n1 = this->isa_node();
            auto n2 = other.isa_node();
            if (n1 && n2) {
                if (n1->min > n2->def->tid() || n1->def->tid() < n2->min) return false;
                if (n1->def == n2->def) return true;
                if (!n1->lca(n2)->is_root()) return true;

                while (!n1->is_root() && !n2->is_root()) {
                    if (n1->def->tid() > n2->def->tid()) {
                        if (n1 = n1->find(n2->def); n2->def == n1->def) return true;
                        n1 = n1->parent;
                    } else {
                        if (n2 = n2->find(n1->def); n1->def == n2->def) return true;
                        n2 = n2->parent;
                    }
                }

                return false;
            }

            auto n = n1 ? n1 : n2;
            for (auto e : *(d1 ? d1 : d2))
                if (n->contains(e)) return true;

            return false;
        }
        ///@}

        /// @name Iterators
        ///@{
        constexpr iterator begin() const noexcept {
            if (auto u = isa_uniq()) return {u};
            if (auto d = isa_data()) return {d->begin()};
            if (auto n = isa_node(); n && !n->is_root()) return {n};
            return {};
        }

        constexpr iterator end() const noexcept {
            if (auto data = isa_data()) return iterator(data->end());
            return {};
        }
        ///@}

        /// @name Comparisons
        ///@{
        constexpr bool operator==(Set other) const noexcept { return this->ptr_ == other.ptr_; }
        ///@}

        /// @name Output
        ///@{
        std::ostream& stream(std::ostream& os) const {
            os << '{';
            auto sep = "";
            for (auto d : *this) {
                os << sep << d->sym() << ": " << d->gid() << '/' << d->tid();
                sep = ", ";
            }
            return os << '}';
        }

        void dump() const { stream(std::cout) << std::endl; }
        ///@}

    private:
        constexpr Tag tag() const noexcept { return Tag(ptr_ & uintptr_t(0b11)); }
        template<class T>
        constexpr T* ptr() const noexcept {
            return std::bit_cast<T*>(ptr_ & ~uintptr_t(0b11));
        }
        // clang-format off
        constexpr D*    isa_uniq() const noexcept { return tag() == Tag::Uniq ? ptr<D   >() : nullptr; }
        constexpr Data* isa_data() const noexcept { return tag() == Tag::Data ? ptr<Data>() : nullptr; }
        constexpr Node* isa_node() const noexcept { return tag() == Tag::Node ? ptr<Node>() : nullptr; }
        // clang-format on

        uintptr_t ptr_ = 0;

        friend class Sets;
        friend std::ostream& operator<<(std::ostream& os, Set set) { return set.stream(os); }
    };

    static_assert(std::forward_iterator<typename Set::iterator>);
    static_assert(std::ranges::range<Set>);

    /// @name Construction
    ///@{
    Sets& operator=(const Sets&) = delete;

    constexpr Sets() noexcept
        : root_(make_node()) {}
    constexpr Sets(const Sets&) noexcept = delete;
    constexpr Sets(Sets&& other) noexcept
        : Sets() {
        swap(*this, other);
    }
    ///@}

    /// @name Set Operations
    /// @note These operations do **not** modify the input set(s); they create a **new** Set.
    ///@{

    /// Create a Set wih all elements in @p v.
    [[nodiscard]] Set create(Vector<D*> v) {
        std::sort(v.begin(), v.end(), gid_lt);
        auto vb   = v.begin();
        auto vu   = std::unique(vb, v.end());
        auto size = std::distance(vb, vu);

        if (size == 0) return {};
        if (size == 1) return {*vb};

        if (size_t(size) <= N) {
            auto [data, state] = allocate(size);
            std::copy(vb, vu, data->begin());
            return unify(data, state);
        }

        return create_trie(vb, vu);
    }

    /// Yields @f$s \cup \{d\}@f$.
    [[nodiscard]] Set insert(Set s, D* d) {
        if (auto u = s.isa_uniq()) {
            if (d == u) return {d};

            auto [data, state] = allocate(2);
            if (d->gid() < u->gid())
                data->elems[0] = d, data->elems[1] = u;
            else
                data->elems[0] = u, data->elems[1] = d;
            return unify(data, state);
        }

        if (auto src = s.isa_data()) {
            auto size = src->size;
            assert(size <= N);

            for (auto e : *src)
                if (d == e) return s; // already here

            if (size == N) { // one more element is too much for a Data set: switch over to the trie
                // Use the data arena as scratch space for the N + 1 elements; since create_trie only draws from
                // node_arena_, it is ours to throw away again afterwards.
                auto [scratch, state] = allocate(N + 1);
                auto o                = std::copy(src->begin(), src->end(), scratch->begin());
                *o++                  = d;
#ifndef NDEBUG
                auto scratch_state = data_arena_.state();
#endif
                auto res = create_trie(scratch->begin(), o);
                assert(scratch_state == data_arena_.state() && "create_trie must only draw from node_arena_");
                data_arena_.deallocate(state);
                return res;
            }

            auto [dst, state] = allocate(size + 1);
            auto i            = std::upper_bound(src->begin(), src->end(), d, gid_lt); // where d belongs
            auto o            = std::copy(src->begin(), i, dst->begin());
            *o++              = d;
            std::copy(i, src->end(), o);
            return unify(dst, state);
        }

        if (auto n = s.isa_node()) {
            if (n->contains(d)) return n;
            return insert(n, d);
        }

        return {d};
    }

    /// Yields @f$s_1 \cup s_2@f$.
    [[nodiscard]] Set merge(Set s1, Set s2) {
        if (s1.empty() || s1 == s2) return s2;
        if (s2.empty()) return s1;

        if (auto u = s1.isa_uniq()) return insert(s2, u);
        if (auto u = s2.isa_uniq()) return insert(s1, u);

        auto d1 = s1.isa_data();
        auto d2 = s2.isa_data();
        if (d1 && d2) {
            // Both operands are ordered by gid and duplicate-free, so a linear merge yields the union directly -
            // no sort, no std::unique pass.
            // Its final size is only known afterwards, so allocate the upper bound `d1->size + d2->size` and merge
            // straight into it; every dropped duplicate leaves one slot of excess at the tail that unify releases
            // again.
            auto [data, state] = allocate(d1->size + d2->size);
            auto i1 = d1->begin(), e1 = d1->end();
            auto i2 = d2->begin(), e2 = d2->end();
            auto o = data->begin();

            while (i1 != e1 && i2 != e2) {
                auto g1 = (*i1)->gid();
                auto g2 = (*i2)->gid();
                if (g1 < g2)
                    *o++ = *i1++;
                else if (g2 < g1)
                    *o++ = *i2++;
                else
                    *o++ = *i1++, ++i2; // drop the duplicate
            }
            o = std::copy(i1, e1, o);
            o = std::copy(i2, e2, o);

            auto size = size_t(o - data->begin());
            if (size > N) { // too big for a Data set: switch over to the trie
#ifndef NDEBUG
                auto scratch_state = data_arena_.state();
#endif
                auto res = create_trie(data->begin(), o); // only draws from node_arena_ ...
                assert(scratch_state == data_arena_.state() && "create_trie must only draw from node_arena_");
                data_arena_.deallocate(state); // ... so data is ours to throw away again
                return res;
            }

            auto excess = data->size - size; // data->size is still the upper bound we allocated
            data->size  = size;
            return unify(data, state, excess);
        }

        auto n1 = s1.isa_node();
        auto n2 = s2.isa_node();
        if (n1 && n2) {
            if (n1->is_descendant_of(n2)) return n1;
            if (n2->is_descendant_of(n1)) return n2;
            return merge(n1, n2);
        }

        auto n = n1 ? n1 : n2;
        for (auto d : *(d1 ? d1 : d2))
            if (!n->contains(d)) n = insert(n, d);
        return n;
    }

    /// Yields @f$s \setminus \{d\}@f$.
    [[nodiscard]] Set erase(Set s, D* d) {
        if (auto u = s.isa_uniq()) return d == u ? Set() : s;

        if (auto data = s.isa_data()) {
            auto b = data->begin(), e = data->end();
            auto i = std::find(b, e, d);
            if (i == e) return s; // not in here

            auto size = data->size - 1;
            if (size == 0) return {};
            if (size == 1) return {i == b ? b[1] : b[0]};

            assert(size <= N);
            auto [new_data, state] = allocate(size);
            std::copy(i + 1, e, std::copy(b, i, new_data->begin())); // copy over, skip i
            return unify(new_data, state);
        }

        if (auto n = s.isa_node()) {
            if (!n->contains(d)) return n;

            auto res = erase(n, d);
            if (res->size > N) return res;

            auto v = Vector<D*>();
            v.reserve(res->size);
            for (auto i = res; !i->is_root(); i = i->parent)
                v.emplace_back(i->def);
            return create(std::move(v));
        }

        return {};
    }
    ///@}

    /// @name DOT output
    void dot() {
        auto of = std::ofstream("trie.dot");
        dot(of);
    }

    void dot(std::ostream& os) const {
        std::print(os, "digraph {{\n");
        std::print(os, "ordering=out;\n");
        std::print(os, "node [shape=box,style=filled];\n");
        root()->dot(os);
        std::print(os, "}}\n");
    }

    friend void swap(Sets& s1, Sets& s2) noexcept {
        using std::swap;
        // clang-format off
        swap(s1.data_arena_,  s2.data_arena_);
        swap(s1.node_arena_,  s2.node_arena_);
        swap(s1.pool_,        s2.pool_);
        swap(s1.root_,        s2.root_);
        swap(s1.tid_counter_, s2.tid_counter_);
        swap(s1.id_counter_ , s2.id_counter_ );
        // clang-format on
    }

private:
    D* set_tid(D* def) noexcept {
        assert(def->tid() == 0);
        def->tid_ = tid_counter_++;
        return def;
    }

    /// Data sets are ordered by `D::gid`.
    static constexpr bool gid_lt(D* d1, D* d2) noexcept { return d1->gid() < d2->gid(); }

    /// @name Data helpers
    ///@{
    std::pair<Data*, fe::Arena::State> allocate(size_t size) {
        auto bytes = sizeof(Data) + size * sizeof(D*);
        auto state = data_arena_.state();
        auto buff  = data_arena_.allocate(bytes, alignof(Data));
        auto data  = new (buff) Data(size);
        return {data, state};
    }

    /// Hash-conses @p data; rolls the arena back to @p state, if an equal Data is already pooled.
    /// Pass the number of trailing elements allocated but not used as @p excess to release them again.
    Set unify(Data* data, fe::Arena::State state, size_t excess = 0) {
        assert(data->size != 0);
        auto [i, ins] = pool_.emplace(data);
        if (ins) {
            data_arena_.deallocate(excess * sizeof(D*)); // data is the arena's most recent allocation
            return Set(data);
        }

        data_arena_.deallocate(state);
        return Set(*i);
    }
    ///@}

    /// Builds a trie Set from the *unique* elements in `[begin, end)`; reorders them in place.
    /// @attention Must only ever draw from node_arena_.
    /// Two callers use data_arena_ as scratch space and rewind it afterwards; drawing from data_arena_ in here
    /// would pop pages that are still live - possibly including Data that pool_ still points at.
    /// Both call sites assert this.
    template<class I>
    [[nodiscard]] Set create_trie(I begin, I end) {
        // Sorting is a performance optimization, not a correctness requirement:
        // insert() restores the canonical increasing-tid path from any insertion order, but only an element whose
        // tid exceeds the current tip mounts in O(1) - otherwise it walks up and re-mounts the suffix in O(depth).
        // Feeding the elements in ascending tid order therefore turns O(k * depth) into O(k) mounts.
        // A tid of 0 goes last because set_tid hands out the next - and hence maximal - counter value.
        std::sort(begin, end, [](D* d1, D* d2) { return d1->tid() != 0 && (d2->tid() == 0 || d1->tid() < d2->tid()); });

        auto res = root();
        for (auto i = begin; i != end; ++i)
            res = insert(res, *i);
        return res;
    }

    // Trie helpers
    constexpr Node* root() const noexcept { return root_.get(); }
    fe::Arena::Ptr<Node> make_node() { return node_arena_.mk<Node>(id_counter_++); }
    fe::Arena::Ptr<Node> make_node(Node* parent, D* def) { return node_arena_.mk<Node>(parent, def, id_counter_++); }

    [[nodiscard]] Node* mount(Node* parent, D* def) {
        assert(def->tid() != 0);
        auto [i, ins] = parent->children.emplace(def, nullptr);
        if (ins) i->second = make_node(parent, def);
        return i->second.get();
    }

    [[nodiscard]] constexpr Node* insert(Node* n, D* d) noexcept {
        if (d->tid() == 0) return mount(n, set_tid(d));
        if (n->def == d) return n;
        if (n->is_root() || n->def->tid() < d->tid()) return mount(n, d);
        return mount(insert(n->parent, d), n->def);
    }

    [[nodiscard]] constexpr Node* merge(Node* n, Node* m) {
        if (n == m || m->is_root()) return n;
        if (n->is_root()) return m;
        auto nn = n->def->tid() < m->def->tid() ? n : n->parent;
        auto mm = n->def->tid() > m->def->tid() ? m : m->parent;
        return mount(merge(nn, mm), n->def->tid() < m->def->tid() ? m->def : n->def);
    }

    [[nodiscard]] Node* erase(Node* n, D* d) {
        if (d->tid() > n->def->tid()) return n;
        if (n->def == d) return n->parent;
        return mount(erase(n->parent, d), n->def);
    }

    fe::Arena node_arena_;
    fe::Arena data_arena_;
    absl::flat_hash_set<const Data*, absl::Hash<const Data*>, typename Data::Equal> pool_;
    fe::Arena::Ptr<Node> root_;
    u32 tid_counter_ = 1;
    u32 id_counter_  = 0;
};

} // namespace mim
