#pragma once

#include <cstddef>
#include <cstdint>

#include <type_traits>

namespace mim {

/// @name Simple Hash
/// See [Wikipedia](https://en.wikipedia.org/wiki/MurmurHash).
///@{
/// Use for a single value to hash.
inline constexpr uint32_t murmur3(uint32_t h) noexcept {
    h ^= h >> UINT32_C(16);
    h *= UINT32_C(0x85ebca6b);
    h ^= h >> UINT32_C(13);
    h *= UINT32_C(0xc2b2ae35);
    h ^= h >> UINT32_C(16);
    return h;
}

inline constexpr uint64_t splitmix64(uint64_t h) noexcept {
    h ^= h >> UINT64_C(30);
    h *= UINT64_C(0xbf58476d1ce4e5b9);
    h ^= h >> UINT64_C(27);
    h *= UINT64_C(0x94d049bb133111eb);
    h ^= h >> UINT64_C(31);
    return h;
}

inline constexpr size_t hash(size_t h) noexcept {
    if constexpr (sizeof(size_t) == 4)
        return murmur3(h);
    else if constexpr (sizeof(size_t) == 8)
        return splitmix64(h);
    else
        static_assert("unsupported size of size_t");
}
///@}

/// @name FNV-1 Hash
/// See [Wikipedia](https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function#FNV-1_hash).
///@{
/// [Magic numbers](http://www.isthe.com/chongo/tech/comp/fnv/index.html#FNV-var) for FNV-1 hash.
template<size_t>
struct FNV1 {};

template<>
struct FNV1<4> {
    static const uint32_t offset = UINT32_C(2166136261);
    static const uint32_t prime  = UINT32_C(16777619);
};

template<>
struct FNV1<8> {
    static const uint64_t offset = UINT64_C(14695981039346656037);
    static const uint64_t prime  = UINT64_C(1099511628211);
};

/// Mixes @p v into @p seed word-wise, reusing the FNV-1 prime as the multiplier.
/// @note Hash values are never serialized - they only feed World::SeaHash and Scheduler's UseHash - so this
/// formulation is free to change; equality is structural, never hash-based.
template<class T>
constexpr size_t hash_combine(size_t seed, T v) noexcept {
    static_assert(std::is_signed<T>::value || std::is_unsigned<T>::value, "please provide your own hash function");

    return hash(seed ^ (size_t(v) * FNV1<sizeof(size_t)>::prime));
}

inline consteval size_t hash_begin() noexcept { return FNV1<sizeof(size_t)>::offset; }

template<class T>
constexpr size_t hash_begin(T val) noexcept {
    return hash_combine(hash_begin(), val);
}
///@}

} // namespace mim
