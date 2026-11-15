#pragma once

#include <algorithm>
#include <limits>

#include "pbqp/graph.h"

namespace pbqp {

/// Any sign; kInfinity absorbs.
inline Cost add_signed(Cost lhs, Cost rhs) {
    if (lhs == kInfinity || rhs == kInfinity) return kInfinity;
    const CostSum sum = CostSum{lhs} + rhs;
    return static_cast<Cost>(std::clamp<CostSum>(sum, std::numeric_limits<Cost>::min(), kInfinity));
}

} // namespace pbqp
