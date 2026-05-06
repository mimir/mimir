#include <iostream>

#include "mim/axm.h"
#include "mim/world.h"

#include "mim/plug/matrix/matrix.h"

// TODO: combine map_reduce calls

namespace mim::plug::matrix {

/// Normalizer for read opertions
/// - read(constMat v) -> v
/// - read(insert m v i, i) -> v (TODO: check with map_reduce)
/// - read(insert m v i, j) -> read(m, i) if i <> j (TODO: wanted? useful?)
/// - read(transpose m, (i,j)) -> read(m, (j,i)) (TODO: check for map_reduce)
/// - read(product m1 m2, (i,j)) -> ... (TODO: check with map_reduce)
/// - read (map_reduce f) idx = loop f idx (TODO: implement => use inner loop from lowering phase)
const Def* normalize_read(const Def* type, const Def*, const Def* arg) {
    auto& world            = type->world();
    auto [mem, mat, index] = arg->projs<3>();

    world.DLOG("normalizing read: mat: {}\n", mat);

    if (auto mex = mat->isa<Extract>()) {
        world.DLOG("  extract: {}\n", mex);
        auto ccall = mex->tuple();
        world.DLOG("  ex_mat: {}\n", ccall);
        if (auto mcm = Axm::isa<constMat>(ccall)) {
            world.DLOG("  const mat: {}\n", mcm);
            auto [cmem, v] = mcm->arg()->projs<2>();
            return world.tuple({mem, v});
        }
    }

    return {};
}

/// Normalizer for write operations
/// TODO: implement
const Def* normalize_insert(const Def*, const Def*, const Def*) { return {}; }

/// Normalizer for transpose operations
/// - transpose (constMat v) -> constMat v (TODO: implement)
/// - transpose (insert m v (i,j)) -> insert (transpose m) v (j,i) (TODO: implement, maybe other way around?)
/// - transpose (tranpose m) -> m (TODO: implement)

/// - shape (\@mat n (k1,k2,...,kn) i) -> (k1,k2,...,kn)\#i (TODO: implement)
const Def* normalize_shape(const Def* type, const Def* callee, const Def* arg) {
    auto& world                   = type->world();
    auto [mat, index]             = arg->projs<2>();
    auto [dims, sizes, body_type] = Axm::isa<Mat, false>(mat->type())->args<3>();
    (void)callee;

    return world.extract(sizes, index);
}

/// Matrix normalizer for product on two-dimensional matrices
/// - product (constMat v1, constMat v2) -> constMat v1 * v2 * dim (TODO: implement)
/// - product (constMat v, m) -> ... (TODO: implement)
/// - product (m, constMat v) -> ... (TODO: implement)
/// - product (id, m) -> m (TODO: check)
/// - product (m, id) -> m

/// - map(constMat v, f) -> constMat f(v) (TODO: implement)
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
