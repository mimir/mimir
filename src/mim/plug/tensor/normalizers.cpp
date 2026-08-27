#include <mim/def.h>
#include <mim/plugin.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include <mim/util/sets.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/tuple/tuple.h>
#include <mim/plug/vec/vec.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor {

// There's no good reason keeping 1s around for get/set indices.
// So this just skips relevant dimensions in the index and shape, and reduces the rank accordingly.
std::tuple<u64, const Def*, const Def*> fold_shape_and_index(const Def* shape, const Def* index) {
    auto& w = shape->world();

    DefVec dims;
    DefVec index_dims;
    auto r = shape->num_projs();
    for (size_t i = 0, e = r; i != e; ++i) {
        auto dim = shape->proj(r, i);
        if (auto dim_lit = Lit::isa<u64>(dim))
            if (dim_lit == 1) continue;

        dims.push_back(dim);
        index_dims.push_back(index->proj(r, i));
    }

    assert(dims.size() == index_dims.size());
    return std::make_tuple(dims.size(), w.tuple(dims), w.tuple(index_dims));
}

const Def* normalize_get(const Def*, const Def* c, const Def* arg) {
    auto& w = c->world();

    auto [index, arr] = arg->projs<2>();
    auto callee       = c->as<App>();
    auto [T, r, s]    = callee->args<3>();

    w.DLOG("normalize_get");
    w.DLOG("    arr = {} : {}", arr, arr->type());
    w.DLOG("    index = {} : {}", index, index->type());
    w.DLOG("    T = {} : {}", T, T->type());
    w.DLOG("    r = {} : {}", r, r->type());
    w.DLOG("    s = {} : {}", s, s->type());

    if (r->isa<Lit>()) {
        auto [new_r, new_s, new_index] = fold_shape_and_index(s, index);
        w.DLOG("    new_index = {} : {}", new_index, new_index->type());
        w.DLOG("    new_s = {} : {}", new_s, new_s->type());
        w.DLOG("    new_r = {} : {}", w.lit_nat(new_r), w.lit_nat(new_r)->type());
        if (new_r == 0) return arr;
        if (new_s != s || new_index != index) return op_get(T, w.lit_nat(new_r), new_s, arr, new_index);
    }

    if (Axm::isa<tensor::set>(arr)) {
        w.DLOG("get after set, try to bypass");
        auto set                  = arr->as<App>();
        auto [target_index, _, x] = set->args<3>();
        if (target_index == index) {
            w.DLOG("bypass successful");
            return x;
        }
    }
    if (Axm::isa<tensor::get>(arr)) {
        w.DLOG("get after get, try to bypass");
        auto get                      = arr->as<App>();
        auto [outer_index, outer_arr] = get->args<2>();
        auto [o_T, o_r, o_s]          = get->callee()->as<App>()->args<3>();
        w.DLOG("    outer_arr = {} : {}", outer_arr, outer_arr->type());
        w.DLOG("    outer_index = {} : {}", outer_index, outer_index->type());
        w.DLOG("    o_T = {} : {}", o_T, o_T->type());
        w.DLOG("    o_r = {} : {}", o_r, o_r->type());
        w.DLOG("    o_s = {} : {}", o_s, o_s->type());

        auto new_r     = w.call(core::nat::add, DefVec{r, o_r});
        auto new_s     = w.call<tuple::cat>(DefVec{o_s, s});
        auto new_index = w.call<tuple::cat>(DefVec{outer_index, index});

        return op_get(T, new_r, new_s, outer_arr, new_index);
    }

    if (auto rep = Axm::isa<tensor::repeat>(arr)) {
        // get after repeat: read the input directly at `idx mod s_in` per axis. Decidable when an axis
        // passes through (`s_in#d == s_out#d`), is size-1 (read at 0), or has a literal extent and a
        // literal index component (fold the mod); otherwise keep the repeat (the lowering paths emit the
        // runtime mod).
        w.DLOG("get after repeat, try to bypass");
        auto input             = rep->arg();
        auto [Tr, s_in, s_out] = rep->callee()->as<App>()->uncurry_args<3>();
        if (auto r_l = Lit::isa<u64>(Tr->proj(2, 1))) {
            DefVec new_index(*r_l);
            for (u64 d = 0; d < *r_l; ++d) {
                auto in_d  = s_in->proj(*r_l, d);
                auto idx_d = index->proj(*r_l, d);
                if (in_d == s_out->proj(*r_l, d))
                    new_index[d] = idx_d;
                else if (auto l = Lit::isa<u64>(in_d); l && *l == 1)
                    new_index[d] = w.lit_idx(1, 0);
                else if (auto e = Lit::isa<u64>(in_d), i = Lit::isa<u64>(idx_d); e && i)
                    new_index[d] = w.lit_idx(*e, *i % *e);
                else
                    return nullptr;
            }
            w.DLOG("bypass successful");
            return op_get(T, Tr->proj(2, 1), s_in, input, w.tuple(new_index));
        }
    }

    if (auto bc = Axm::isa<tensor::broadcast>(arr)) {
        // get after broadcast: read the input directly. Per axis, the broadcast either passes the index
        // through (`s_in#d == s_out#d`) or reads a size-1 input axis at 0; if some axis is neither
        // decidably equal nor literal 1, keep the broadcast.
        w.DLOG("get after broadcast, try to bypass");
        auto [s_in, s_out, input] = bc->args<3>();
        auto [b_T, b_r]           = bc->callee()->as<App>()->args<2>();
        if (auto r_l = Lit::isa<u64>(b_r)) {
            DefVec new_index(*r_l);
            for (u64 d = 0; d < *r_l; ++d) {
                auto in_d = s_in->proj(*r_l, d);
                if (in_d == s_out->proj(*r_l, d))
                    new_index[d] = index->proj(*r_l, d);
                else if (auto l = Lit::isa<u64>(in_d); l && *l == 1)
                    new_index[d] = w.lit_idx(1, 0);
                else
                    return nullptr;
            }
            w.DLOG("bypass successful");
            return op_get(T, b_r, s_in, input, w.tuple(new_index));
        }
    }

    return nullptr;
}

const Def* normalize_set(const Def*, const Def* c, const Def* arg) {
    auto& w = c->world();

    auto [index, arr, x] = arg->projs<3>();
    w.DLOG("normalize_set");
    w.DLOG("    arr = {} : {}", arr, arr->type());
    w.DLOG("    index = {} : {}", index, index->type());
    w.DLOG("    x = {} : {}", x, x->type());

    auto callee    = c->as<App>();
    auto [T, r, s] = callee->args<3>();

    if (r->isa<Lit>()) {
        auto [new_r, new_s, new_index] = fold_shape_and_index(s, index);
        w.DLOG("    new_index = {} : {}", new_index, new_index->type());
        w.DLOG("    new_s = {} : {}", new_s, new_s->type());
        w.DLOG("    new_r = {} : {}", w.lit_nat(new_r), w.lit_nat(new_r)->type());
        if (new_r == 0) return x;
        if (new_s != s || new_index != index) return op_set(T, w.lit_nat(new_r), new_s, arr, new_index, x);
    }

    if (Axm::isa<tensor::get>(x)) {
        w.DLOG("set after get, try to bypass");
        auto get                      = x->as<App>();
        auto [inner_index, inner_arr] = get->args<2>();
        if (inner_arr == arr && inner_index == index) {
            w.DLOG("bypass successful");
            return inner_arr;
        }
    }

    if (Axm::isa<tensor::set>(x)) {
        w.DLOG("set after set, try to bypass");
        auto inner_set                         = x->as<App>();
        auto [inner_index, inner_arr, inner_x] = inner_set->args<3>();
        auto [i_T, i_r, i_s]                   = inner_set->callee()->as<App>()->args<3>();

        w.DLOG("    inner_arr = {} : {}", inner_arr, inner_arr->type());
        w.DLOG("    inner_index = {} : {}", inner_index, inner_index->type());
        w.DLOG("    inner_x = {} : {}", inner_x, inner_x->type());
        w.DLOG("    i_T = {} : {}", i_T, i_T->type());
        w.DLOG("    i_r = {} : {}", i_r, i_r->type());
        w.DLOG("    i_s = {} : {}", i_s, i_s->type());

        if (auto inner_get = Axm::isa<tensor::get>(inner_arr)) {
            auto [g_index, g_arr] = inner_get->args<2>();
            if (g_arr == arr && g_index == index) {
                auto new_r     = w.call(core::nat::add, DefVec{r, i_r});
                auto new_s     = w.call<tuple::cat>(DefVec{s, i_s});
                auto new_index = w.call<tuple::cat>(DefVec{index, inner_index});

                return op_set(i_T, new_r, new_s, arr, new_index, inner_x);
            }
        }
        w.DLOG("set after set bypass not applicable: inner_arr is not get(arr, index)");
    }
    w.DLOG("no normalization applicable");
    return nullptr;
}

const Def* normalize_broadcast(const Def*, const Def* c, const Def* arg) {
    auto& w = c->world();

    auto [s_in, s_out, input] = arg->projs<3>();
    auto callee               = c->as<App>();
    auto [T, r]               = callee->args<2>();
    w.DLOG("normalize_broadcast");
    w.DLOG("    s_out = {} : {}", s_out, s_out->type());
    w.DLOG("    input = {} : {}", input, input->type());
    w.DLOG("    T = {} : {}", T, T->type());
    w.DLOG("    r = {} : {}", r, r->type());
    w.DLOG("    s_in = {} : {}", s_in, s_in->type());

    if (s_in == s_out) return input;

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) return nullptr;
    if (r_nat == 0) return input;

    return nullptr;
}

const Def* normalize_broadcast_in_dim(const Def*, const Def*, const Def*) { return nullptr; }

const Def* normalize_repeat(const Def*, const Def* c, const Def* arg) {
    // Identity repeat: if the input and output shapes agree, the repeat is a no-op.
    auto [Tr, s_in, s_out] = c->as<App>()->uncurry_args<3>();
    if (s_in == s_out) return arg;
    return nullptr;
}

const Def* normalize_reshape(const Def*, const Def* c, const Def* arg) {
    // Identity reshape: if the input and output shapes agree, the reshape is a no-op.
    auto [Trr, s_in, s_out] = c->as<App>()->uncurry_args<3>();
    if (s_in == s_out) return arg;
    return nullptr;
}

const Def* normalize_slice(const Def*, const Def* c, const Def* arg) {
    // Identity slice: every axis starts at 0 with step 1 and keeps its full extent (s_out == s_in) -> the input itself.
    auto [Tr, s_in, params]   = c->as<App>()->uncurry_args<3>();
    auto [start, step, s_out] = params->projs<3>();
    if (s_out != s_in) return nullptr;
    auto r = Lit::isa<u64>(Tr->proj(2, 1));
    if (!r) return nullptr;
    for (u64 d = 0; d != *r; ++d) {
        auto st = Lit::isa<u64>(start->proj(*r, d));
        auto sp = Lit::isa<u64>(step->proj(*r, d));
        if (!st || *st != 0 || !sp || *sp != 1) return nullptr;
    }
    return arg;
}

const Def* normalize_flip(const Def*, const Def*, const Def*) { return nullptr; }

const Def* normalize_pad(const Def*, const Def* c, const Def* arg) {
    // Identity pad: every axis has lo == hi == 0 (so s_out == s_in) -> the input itself (the fill value is irrelevant).
    auto [Tr, s_in, params] = c->as<App>()->uncurry_args<3>();
    auto [mode, lo, hi]     = params->projs<3>();
    auto r                  = Lit::isa<u64>(Tr->proj(2, 1));
    if (!r) return nullptr;
    for (u64 d = 0; d != *r; ++d) {
        auto l = Lit::isa<u64>(lo->proj(*r, d));
        auto h = Lit::isa<u64>(hi->proj(*r, d));
        if (!l || *l != 0 || !h || *h != 0) return nullptr;
    }
    return arg->proj(2, 0); // input (arg = (input, value))
}

const Def* normalize_concat(const Def*, const Def*, const Def*) { return nullptr; }

const Def* normalize_if_static(const Def*, const Def*, const Def* arg) {
    // `%tensor.if_static (k, s, d)` picks `s` once `k` has folded to a literal; a still-symbolic `k`
    // keeps the App stuck, and the tensor lowerings residualize it to `d` (by lowering time,
    // undecided means runtime).
    auto [k, s, d] = arg->projs<3>();
    if (Lit::isa(k)) return s;
    return nullptr;
}

const Def* normalize_fastest_axis(const Def*, const Def*, const Def* arg) {
    // `%tensor.fastest_axis (r, t)` reflects which axis of `t` is the fastest-varying (unit-stride)
    // axis of the tensor actually read once `fuse_tensor`'s read-through has absorbed a pure
    // re-indexed read behind `t`: without one, `t`'s own last axis; behind one, found by evaluating
    // the read's access map on distinct `%affine.lit` markers — normalization folds the map's
    // extracts over the marker tuple, and a last component that does not fold back to a marker
    // (reshape arithmetic) stays unknown. Unknown answers the sentinel `r`.
    auto& w     = arg->world();
    auto [r, t] = arg->projs<2>();
    auto r_l    = Lit::isa<u64>(r);
    if (!r_l || *r_l == 0) return r;
    auto pr = is_pure_read(t);
    if (!pr) return w.lit_nat(*r_l - 1);
    // One level only: a source that is itself absorbed would need the composed analysis.
    if (is_pure_read(pr->src) || Axm::isa<tensor::broadcast>(pr->src)) return r;
    auto r_src = Lit::isa<u64>(pr->map->type()->as<Pi>()->codom()->arity());
    if (!r_src || *r_src == 0) return r;
    // `r` is not type-coupled to `t`; a mismatched rank must answer unknown, not break the app below.
    auto r_dom = Lit::isa<u64>(pr->map->type()->as<Pi>()->dom()->arity());
    if (!r_dom || *r_dom != *r_l) return r;
    // Markers start at 1: 0 is what a broadcast map's `o#d · 0` folds to, so it must not be one.
    auto markers = DefVec(*r_l, [&](size_t i) { return w.call<affine::lit>(w.lit_nat(i + 1)); });
    auto last    = w.app(pr->map, w.tuple(markers))->proj(*r_src, *r_src - 1);
    auto c       = Axm::isa<affine::lit>(last);
    if (!c) return r;
    auto v = Lit::isa<u64>(c->arg());
    if (!v || *v < 1 || *v > *r_l) return r;
    return w.lit_nat(*v - 1);
}

const Def* normalize_shape(const Def*, const Def* c, const Def* arg) {
    // `%tensor.shape r arr` reads the shape off `arr`'s (nested array) type by peeling `r` levels.
    auto& w = c->world();
    auto r  = Lit::isa<u64>(c->as<App>()->arg()); // the explicit rank `r`
    if (!r) return nullptr;

    DefVec dims;
    auto ty = arg->type();
    for (u64 i = 0; i != *r; ++i)
        if (auto a = ty->isa<Seq>()) {
            dims.emplace_back(a->arity());
            ty = a->body();
        } else
            return nullptr; // `arr` is not (statically) a rank-`r` array
    return w.tuple(dims);   // the per-axis sizes; for a rectangular tensor each `arity()` is a plain Nat
}

MIM_tensor_NORMALIZER_IMPL

} // namespace mim::plug::tensor
