#include "mim/plug/tensor/phase/constraints.h"

namespace mim::plug::tensor::phase {

namespace {

bool statically_le(const Def* lhs, const Def* rhs) {
    if (lhs == rhs) return true;
    auto l = Lit::isa<u64>(lhs);
    auto r = Lit::isa<u64>(rhs);
    return l && r && *l <= *r;
}

bool statically_violates(const Def* lhs, const Def* rhs) {
    auto l = Lit::isa<u64>(lhs);
    auto r = Lit::isa<u64>(rhs);
    return l && r && *l > *r;
}

} // namespace

bool check_gather_shape_constraints(const Def* rank, const Def* dim, const Def* src_shape, const Def* idx_shape) {
    auto rank_l = Lit::isa<u64>(rank);
    auto dim_l  = Lit::isa<u64>(dim);
    if (!rank_l || !dim_l) return false;

    bool proven = true;
    for (u64 d = 0; d < *rank_l; ++d)
        if (d != *dim_l) {
            auto lhs = idx_shape->proj(*rank_l, d);
            auto rhs = src_shape->proj(*rank_l, d);
            if (statically_violates(lhs, rhs)) fe::throwf("gather index shape exceeds input shape at axis {}", d);
            proven &= statically_le(lhs, rhs);
        }
    return proven;
}

bool check_scatter_shape_constraints(const Def* rank,
                                     const Def* dim,
                                     const Def* src_shape,
                                     const Def* idx_shape,
                                     const Def* updates_shape) {
    auto rank_l = Lit::isa<u64>(rank);
    auto dim_l  = Lit::isa<u64>(dim);
    if (!rank_l || !dim_l) return false;

    bool proven = true;
    for (u64 d = 0; d < *rank_l; ++d) {
        auto idx = idx_shape->proj(*rank_l, d);
        auto upd = updates_shape->proj(*rank_l, d);
        if (d != *dim_l) {
            auto src = src_shape->proj(*rank_l, d);
            if (statically_violates(idx, src)) fe::throwf("scatter index shape exceeds input shape at axis {}", d);
            proven &= statically_le(idx, src);
        }
        if (statically_violates(idx, upd)) fe::throwf("scatter index shape exceeds updates shape at axis {}", d);
        proven &= statically_le(idx, upd);
    }
    return proven;
}

} // namespace mim::plug::tensor::phase
