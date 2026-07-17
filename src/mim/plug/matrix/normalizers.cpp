#include "mim/axm.h"
#include "mim/world.h"

#include "mim/plug/matrix/matrix.h"

// TODO: combine map_reduce calls
// The element-level normalizers (read / insert / shape) now live in the `buffer` plugin.

namespace mim::plug::matrix {

/// Matrix normalizer for product on two-dimensional matrices
/// - product (constant v1, constant v2) -> constant v1 * v2 * dim (TODO: implement)
/// - product (constant v, m) -> ... (TODO: implement)
/// - product (m, constant v) -> ... (TODO: implement)
/// - product (id, m) -> m (TODO: check)
/// - product (m, id) -> m

/// - map(constant v, f) -> constant f(v) (TODO: implement)
/// - map f (map g m) -> map (f . g) m (TODO: implement)
/// - map f (zipWith g m1 m2) -> zipWith (f . g) m1 m2 (TODO: implement)
u64 get_max_index(u64 init, Defs inputs) {
    auto max_idx = init;

    for (auto inp : inputs) {
        auto [indices, mat] = inp->projs<2>();
        auto indice_count   = Lit::isa(indices->arity());
        if (!indice_count) return -1;
        for (auto idx : indices->projs()) {
            auto idx_val = Lit::isa(idx);
            if (!idx_val) return -1;
            if (idx_val > max_idx) max_idx = idx_val.value();
        }
    }

    return max_idx;
}

/// map_reduce normalizers
/// - TODO: map_reduce (..., ((idx,map_reduce([out, ]...), ...))) -> unify idx, out (out is implicit), name vars apart
///   requires: same reduction, distributive reduction
/// we assume distributivity of the reduction function
const Def* normalize_map_reduce(const Def*, const Def*, const Def*) { return {}; }
const Def* normalize_prod(const Def*, const Def*, const Def*) { return {}; }
const Def* normalize_transpose(const Def*, const Def*, const Def*) { return {}; }

MIM_matrix_NORMALIZER_IMPL

} // namespace mim::plug::matrix
