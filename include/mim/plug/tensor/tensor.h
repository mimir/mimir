#pragma once

#include <optional>

#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/tensor/autogen.h"

namespace mim::plug::tensor {

/// Recognizes the (rebuilt) `tensor_copy` combiner `(acc, ys) ↦ ys#0`: the result is exactly the
/// single input element, so a map_reduce built on it is a pure re-indexed read of that input.
inline bool is_copy_comb(const Def* comb) {
    auto lam = comb->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return false;
    auto app = lam->body()->isa<App>();
    return app && app->callee() == lam->var(1) && app->arg() == lam->var(0)->proj(2, 1)->proj(1, 0);
}

/// Is `post` the (rebuilt) CPS identity `%tensor.id`, i.e. a lam `(x, extras) ↦ x` that returns its
/// first argument (and hence has no epilogue inputs)?
inline bool is_identity_post(const Def* post) {
    auto lam = post->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return false;
    auto app = lam->body()->isa<App>();
    return app && app->callee() == lam->var(1) && app->arg() == lam->var(0)->proj(2, 0);
}

/// A pure re-indexed read: the source tensor, the access map into it (over the read's output
/// coordinates), and the source's element type/rank/shape.
struct PureRead {
    const Def* src = nullptr;
    const Def* map = nullptr;
    const Def* T   = nullptr;
    const Def* R   = nullptr;
    const Def* S   = nullptr;
};

/// If `value` is a pure re-indexed read — a copy-combiner map_reduce without reduction loops that
/// writes its full loop domain through the identity output map (reshape/transpose/slice/flip/repeat
/// lower to these) — returns its single access map and source.
/// `fuse_tensor`'s read-through absorbs exactly these into the consuming op's access maps.
inline std::optional<PureRead> is_pure_read(const Def* value) {
    auto mr = Axm::isa<tensor::map_reduce_post>(value);
    if (!mr) return {};
    auto [nis_nps, meta, shapes, in_tys, comb_init, map_out, maps_all, is_all] = mr->uncurry_args<8>();
    auto nis_l                                                                 = Lit::isa<u64>(nis_nps->proj(2, 0));
    auto nps_l                                                                 = Lit::isa<u64>(nis_nps->proj(2, 1));
    // No reduction loops: the total loop count Rn equals the output rank Ro.
    if (!nis_l || *nis_l != 1 || !nps_l || *nps_l != 0 || meta->proj(5, 2) != meta->proj(5, 3)) return {};
    auto [So, Sr, sched] = shapes->projs<3>();
    if (Sr != So) return {};
    auto id_lam = map_out->isa_mut<Lam>();
    if (!id_lam || !id_lam->is_set() || id_lam->body() != id_lam->var()) return {};
    auto [comb, init, post] = comb_init->projs<3>();
    if (!is_copy_comb(comb) || !is_identity_post(post)) return {};

    return PureRead{is_all->proj(2, 0)->proj(1, 0), maps_all->proj(2, 0)->proj(1, 0), in_tys->proj(6, 0)->proj(1, 0),
                    in_tys->proj(6, 1)->proj(1, 0), in_tys->proj(6, 2)->proj(1, 0)};
}

/// @note `index` comes *before* `arr` in the operand tuple, see %%tensor.get.
inline const Def* op_get(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index) {
    auto& w = arr->world();
    auto f  = w.annex<tensor::get>();
    f       = w.app(f, {T, r, s});
    f       = w.app(f, {index, arr});
    return f;
}

/// @note `index` comes *before* `arr` in the operand tuple, see %%tensor.get.
inline const Def* op_set(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index, const Def* x) {
    auto& w = arr->world();
    auto f  = w.app(w.annex<tensor::set>(), {T, r, s});
    f       = w.app(f, {index, arr, x});
    return f;
}

} // namespace mim::plug::tensor
