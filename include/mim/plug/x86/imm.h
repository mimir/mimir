#pragma once

#include <climits>

#include <algorithm>
#include <bit>
#include <optional>

#include <mim/def.h>

namespace mim::plug::x86 {

/// Width of the register holding an `Idx size`.
inline nat_t reg_width(nat_t size) { return std::bit_ceil(std::max<nat_t>(Idx::size2bitwidth(size), 8)); }

inline bool fits_i32(s64 x) { return INT32_MIN <= x && x <= INT32_MAX; }

/// Does the literal @p val fit a sign-extended 32-bit immediate, if it is an `Idx size`?
inline bool fits_imm(std::optional<nat_t> size, nat_t val) {
    return (size && reg_width(*size) < 64) || fits_i32(s64(val));
}

} // namespace mim::plug::x86
