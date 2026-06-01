#pragma once

#include <cstdint>
#include <cstring>

#include <algorithm>
#include <bit>
#include <iterator>
#include <queue>
#include <stack>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <fe/assert.h>

#include "mim/util/hash.h"

namespace mim {

/// @name Utility Functions
///@{

/// A bitcast from @p src of type @p S to @p D, supporting different sizes.
template<class D, class S>
constexpr D bitcast_resize(const S& src) noexcept
    requires(std::is_trivially_copyable_v<S> && std::is_trivially_copyable_v<D>)
{
    if constexpr (sizeof(D) == sizeof(S)) {
        return std::bit_cast<D>(src);
    } else {
        D dst{}; // zero-fill
        constexpr std::size_t n = (sizeof(D) < sizeof(S)) ? sizeof(D) : sizeof(S);
        std::memcpy(&dst, &src, n);
        return dst;
    }
}

constexpr std::uint64_t pad(std::uint64_t offset, std::uint64_t align) noexcept {
    assert(align != 0);

    auto mod = offset % align;
    if (mod) offset += align - mod;
    return offset;
}
///@}

/// @name Algorithms
///@{
template<class I, class T, class L>
constexpr I binary_find(I begin, I end, T val, L lt) noexcept {
    static_assert(std::random_access_iterator<I>);
    I i;
    if (std::distance(begin, end) < 16)
        for (i = begin; i != end && lt(*i, val); ++i) {}
    else
        i = std::lower_bound(begin, end, val, lt);
    return (i != end && !lt(val, *i)) ? i : end;
}

/// Like `std::string::substr`, but works on `std::string_view` instead.
constexpr std::string_view subview(std::string_view s, size_t i, size_t n = std::string_view::npos) noexcept {
    n = std::min(n, s.size());
    return {s.data() + i, n - i};
}

/// Replaces all occurrences of @p what with @p repl.
inline void find_and_replace(std::string& str, std::string_view what, std::string_view repl) {
    for (size_t pos = str.find(what); pos != std::string::npos; pos = str.find(what, pos + repl.size()))
        str.replace(pos, what.size(), repl);
}
///@}

/// @name Helpers for Containers
///@{
template<class S>
auto pop(S& s) -> decltype(s.top(), typename S::value_type()) {
    auto val = s.top();
    s.pop();
    return val;
}

template<class Q>
auto pop(Q& q) -> decltype(q.front(), typename Q::value_type()) {
    auto val = q.front();
    q.pop();
    return val;
}

/// Yields pointer to element (or the element itself if it is already a pointer), if found and `nullptr` otherwise.
/// @warning If the element is **not** already a pointer, this lookup will simply take the address of this element.
/// This means that, e.g., a rehash of an `absl::flat_hash_map` will invalidate this pointer.
template<class C, class K>
auto lookup(const C& container, const K& key) {
    auto i = container.find(key);
    if constexpr (std::is_pointer_v<typename C::mapped_type>)
        return i != container.end() ? i->second : nullptr;
    else
        return i != container.end() ? &i->second : nullptr;
}

/// Looks up @p key in @p container, asserts that it exists, and returns the mapped value.
template<class C, class K>
auto assert_lookup(C& container, const K& key) {
    auto i = container.find(key);
    assert(i != container.end());
    return i->second;
}

/// Invokes `emplace` on @p container, asserts that insertion actually happened, and returns the iterator.
template<class C, class... Args>
auto assert_emplace(C& container, Args&&... args) {
    auto [i, ins] = container.emplace(std::forward<Args>(args)...);
    assert_unused(ins);
    return i;
}
///@}

template<class Set>
class unique_queue {
public:
    using T = typename std::remove_reference_t<Set>::value_type;

    unique_queue() = default;
    unique_queue(Set set)
        : done_(set) {}

    bool push(T val) {
        if (done_.emplace(val).second) {
            queue_.emplace(val);
            return true;
        }
        return false;
    }

    bool empty() const { return queue_.empty(); }
    T pop() { return mim::pop(queue_); }
    T& front() { return queue_.front(); }
    T& back() { return queue_.back(); }
    void clear() {
        done_.clear();
        queue_ = {};
    }

private:
    Set done_;
    std::queue<T> queue_;
};

template<class T>
struct GIDHash {
    constexpr size_t operator()(T p) const noexcept { return hash(p->gid()); }
};

template<class T>
struct GIDLt {
    constexpr bool operator()(T a, T b) const noexcept { return a->gid() < b->gid(); }
};

// clang-format off
/// @name GID
///@{
template<class K, class V> using GIDMap     = absl::flat_hash_map<K, V, GIDHash<K>>;
template<class K>          using GIDSet     = absl::flat_hash_set<K,    GIDHash<K>>;
template<class K, class V> using GIDNodeMap = absl::node_hash_map<K, V, GIDHash<K>>;
template<class K>          using GIDNodeSet = absl::node_hash_set<K,    GIDHash<K>>;
///@}

} // namespace mim
