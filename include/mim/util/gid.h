#pragma once

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <fe/hash.h>

namespace mim {

template<class T>
struct GIDHash {
    constexpr size_t operator()(T p) const noexcept { return fe::hash(p->gid()); }
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
// clang-format on

} // namespace mim
