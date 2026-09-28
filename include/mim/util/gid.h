#pragma once

#include <unordered_map>
#include <unordered_set>

#include <ankerl/unordered_dense.h>

namespace mim {

template<class T>
struct GIDHash {
    using is_avalanching = void;

    size_t operator()(T p) const noexcept { return ankerl::unordered_dense::hash<decltype(p->gid())>{}(p->gid()); }
};

template<class T>
struct GIDLt {
    constexpr bool operator()(T a, T b) const noexcept { return a->gid() < b->gid(); }
};

// clang-format off
/// @name GID
///@{
template<class K, class V> using GIDMap     = ankerl::unordered_dense::map<K, V, GIDHash<K>>;
template<class K>          using GIDSet     = ankerl::unordered_dense::set<K,    GIDHash<K>>;
template<class K, class V> using GIDNodeMap = std::unordered_map<K, V, GIDHash<K>>;
template<class K>          using GIDNodeSet = std::unordered_set<K,    GIDHash<K>>;
///@}
// clang-format on

} // namespace mim
