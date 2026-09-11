#pragma once

#include <optional>

#include <fe/worklist.h>

#include <mim/lam.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include "mim/plug/tensor/autogen.h"

namespace mim::plug::tensor {

/// Recognizes the (rebuilt) `tensor_copy` combiner `(acc, ys) ↦ ys#0`: the result is exactly the
/// single input element, so a map_reduce built on it is a pure re-indexed read of that input.
inline bool is_copy_comb(const Def* comb) {
    auto ret = Lam::isa_ret_arg(comb);
    return ret && ret == comb->as_mut<Lam>()->var(0)->proj(2, 1)->proj(1, 0);
}

/// Is `post` the (rebuilt) CPS identity `tensor.id`, i.e. a lam `(x, extras) ↦ x` that returns its
/// first argument (and hence has no epilogue inputs)?
inline bool is_identity_post(const Def* post) {
    auto ret = Lam::isa_ret_arg(post);
    return ret && ret == post->as_mut<Lam>()->var(0)->proj(2, 0);
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

    auto [nis, nps] = nis_nps->projs<2>();
    // No reduction loops: the total loop count Rn equals the output rank Ro.
    if (Lit::isa(nis) != 1 || Lit::isa(nps) != 0 || meta->proj(5, 2) != meta->proj(5, 3)) return {};
    auto [So, Sr, sched] = shapes->projs<3>();
    if (Sr != So) return {};
    auto id_lam = map_out->isa_mut<Lam>();
    if (!id_lam || !id_lam->is_set() || id_lam->body() != id_lam->var()) return {};
    auto [comb, init, post] = comb_init->projs<3>();
    if (!is_copy_comb(comb) || !is_identity_post(post)) return {};

    auto sole = [](const Def* d, u64 n, u64 i) { return d->proj(n, i)->proj(1, 0); };
    return PureRead{sole(is_all, 2, 0), sole(maps_all, 2, 0), sole(in_tys, 6, 0), sole(in_tys, 6, 1),
                    sole(in_tys, 6, 2)};
}

/// @note `index` comes *before* `arr` in the operand tuple, see tensor.get.
inline const Def* op_get(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index) {
    auto& w = arr->world();
    return w.app(w.app(w.annex<tensor::get>(), {T, r, s}), {index, arr});
}

/// @note `index` comes *before* `arr` in the operand tuple, see tensor.get.
inline const Def* op_set(const Def* T, const Def* r, const Def* s, const Def* arr, const Def* index, const Def* x) {
    auto& w = arr->world();
    return w.app(w.app(w.annex<tensor::set>(), {T, r, s}), {index, arr, x});
}

/// Counts the consumers of every def of @p world matched by @p pred.
/// Tuples and packs are transparent argument wrappers, so a wrapped def is charged to the enclosing
/// non-tuple consumer - a shared argument tuple charges each of its users, and a def used twice in one
/// argument list counts twice.
/// A phase whose world does not track uses needs this up front.
template<class Pred>
DefMap<u64> count_consumers(const World& world, Pred pred) {
    auto counts = DefMap<u64>();
    auto charge = [&](this auto&& charge, const Def* d) -> void {
        if (pred(d))
            ++counts[d];
        else if (d->isa<Tuple>() || d->isa<Pack>())
            for (auto op : d->ops())
                if (op) charge(op);
    };

    auto wl = fe::BFSWorklist<DefSet>();
    wl.push(world.roots());

    while (!wl.empty()) {
        auto def         = wl.pop();
        auto transparent = def->isa<Tuple>() || def->isa<Pack>();
        for (auto op : def->ops())
            if (op) {
                if (!transparent) charge(op);
                wl.push(op);
            }
        if (def->type()) wl.push(def->type());
    }

    return counts;
}

} // namespace mim::plug::tensor
