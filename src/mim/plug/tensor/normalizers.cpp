#include <algorithm>
#include <array>

#include <fe/format.h>

#include <mim/def.h>
#include <mim/driver.h>
#include <mim/plugin.h>
#include <mim/tuple.h>
#include <mim/world.h>

#include <mim/plug/affine/affine.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor {

namespace {

const Def* read_through(const Def* base, const Def* index) {
    auto& w = base->world();
    const Def *input, *s_in, *s_out, *rank;
    bool wraps;
    if (auto rep = Axm::isa<tensor::repeat>(base)) {
        auto [Tr, in, out] = rep->callee()->as<App>()->uncurry_args<3>();
        input = rep->arg(), s_in = in, s_out = out, rank = Tr->proj(2, 1), wraps = true;
    } else if (auto bc = Axm::isa<tensor::broadcast>(base)) {
        auto [in, out, i] = bc->args<3>();
        auto [_, r]       = bc->callee()->as<App>()->args<2>();
        input = i, s_in = in, s_out = out, rank = r, wraps = false;
    } else {
        return nullptr;
    }
    auto r = Lit::isa<u64>(rank);
    if (!r) return nullptr;

    auto mapped = DefVec(*r);
    for (u64 d = 0; d != *r; ++d) {
        auto in  = s_in->proj(*r, d);
        auto out = s_out->proj(*r, d);
        auto idx = index->proj(*r, d);
        if (in == out)
            mapped[d] = idx;
        else if (Lit::isa(in) == 1)
            mapped[d] = w.lit_idx(1, 0);
        else if (auto e = Lit::isa(in), x = Lit::isa(idx); wraps && e && *e && x)
            mapped[d] = w.lit_idx(*e, *x % *e);
        else
            return nullptr;
    }
    return op_get(input, w.tuple(mapped));
}

} // namespace

const Def* normalize_get(const Def*, const Def* callee, const Def* arg) {
    auto [tensor, index] = arg->projs<2>();
    if (auto array = Axm::isa<tensor::from_array>(tensor)) return arg->world().extract(array->arg(), index);
    if (auto scalar = Axm::isa<tensor::scalar>(tensor)) return scalar->arg();
    if (auto set = Axm::isa<tensor::set>(tensor)) {
        auto [base, written, value] = set->args<3>();
        if (written == index) return value;
        if (auto rank = Lit::isa<u64>(callee->as<App>()->arg(3, 0))) {
            auto a = lit_projs(written, *rank);
            auto b = lit_projs(index, *rank);
            if (a && b && *a != *b) return op_get(base, index);
        }
    }
    return read_through(tensor, index);
}

const Def* normalize_set(const Def*, const Def* callee, const Def* arg) {
    auto [tensor, index, value] = arg->projs<3>();
    if (auto array = Axm::isa<tensor::from_array>(tensor)) {
        auto data = array->arg();
        auto rank = Lit::isa<u64>(callee->as<App>()->arg(3, 0));
        if (data->is_closed() && rank && lit_projs(index, *rank) && value->isa<Lit>()) {
            auto& w = arg->world();
            auto op = w.app(w.annex<tensor::from_array>(), callee->as<App>()->arg());
            return w.app(op, w.insert(data, index, value));
        }
    }
    return nullptr;
}

const Def* normalize_scalar(const Def* type, const Def*, const Def* arg) {
    if (auto item = Axm::isa<tensor::item>(arg))
        if (item->arg()->type() == type) return item->arg();
    return nullptr;
}

const Def* normalize_item(const Def*, const Def*, const Def* arg) {
    if (auto scalar = Axm::isa<tensor::scalar>(arg)) return scalar->arg();
    if (auto array = Axm::isa<tensor::from_array>(arg)) return array->arg();
    if (auto set = Axm::isa<tensor::set>(arg)) return set->arg()->proj(3, 2);
    return nullptr;
}

const Def* normalize_broadcast(const Def* type, const Def* c, const Def* arg) {
    auto& w = c->world();

    auto [s_in, s_out, input] = arg->projs<3>();
    auto callee               = c->as<App>();
    auto [T, r]               = callee->args<2>();
    w.log().d("broadcast: input = {}: {}, T = {}, r = {}, s_in = {}, s_out = {}", input, input->type(), T, r, s_in,
              s_out);

    if (s_in == s_out || input->type() == type) return input;

    return nullptr;
}

const Def* normalize_broadcast_in_dim(const Def*, const Def*, const Def*) { return nullptr; }

const Def* normalize_repeat(const Def* type, const Def* c, const Def* arg) {
    // Identity repeat: if the input and output shapes agree, the repeat is a no-op.
    auto [Tr, s_in, s_out] = c->as<App>()->uncurry_args<3>();
    if (s_in == s_out || arg->type() == type) return arg;
    return nullptr;
}

const Def* normalize_reshape(const Def* type, const Def* c, const Def* arg) {
    // Identity reshape: if the input and output shapes agree, the reshape is a no-op.
    auto [Trr, s_in, s_out] = c->as<App>()->uncurry_args<3>();
    if (s_in == s_out || arg->type() == type) return arg;
    return nullptr;
}

const Def* normalize_transpose(const Def*, const Def* c, const Def* arg) {
    auto callee = c->as<App>()->decurry();
    auto perm   = lit_perm(callee->arg());
    if (!perm) return nullptr;
    auto r = perm->size();
    if (std::ranges::equal(*perm, std::views::iota(u64(0), u64(r)))) return arg;

    auto app = Axm::isa<tensor::transpose>(arg);
    if (!app) return nullptr;
    auto inner = transpose_perm(app);
    if (!inner || inner->size() != r) return nullptr;

    // Axis `j` of the inner input ends up at `perm#(inner#j)`; an identity composite collapses in the rebuilt app.
    auto& w   = c->world();
    auto comp = DefVec(r, [&](size_t j) { return w.lit_idx(r, (*perm)[(*inner)[j]]); });
    return w.app(w.app(w.app(callee->callee(), w.tuple(comp)), app->decurry()->arg()), app->arg());
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
    // `tensor.if_static (k, s, d)` picks `s` once `k` has folded to a literal; a still-symbolic `k`
    // keeps the App stuck, and the tensor lowerings residualize it to `d` (by lowering time,
    // undecided means runtime).
    auto [k, s, d] = arg->projs<3>();
    if (Lit::isa(k)) return s;
    return nullptr;
}

const Def* normalize_fastest_axis(const Def*, const Def*, const Def* arg) {
    // `tensor.fastest_axis (r, t)` reflects which axis of `t` is the fastest-varying (unit-stride)
    // axis of the tensor actually read once `fuse_tensor`'s read-through has absorbed a pure
    // re-indexed read behind `t`: without one, `t`'s own last axis; behind one, found by evaluating
    // the read's access map on distinct `affine.lit` markers — normalization folds the map's
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

/// The vector widths `-X tensor:vec-width` accepts.
constexpr std::array Vec_widths = {1_u64, 4_u64, 8_u64, 16_u64};

const Def* normalize_vec_width(const Def*, const Def*, const Def* arg) {
    // Read off the Driver so one graph can be re-emitted for another target by another Driver.
    auto& w = arg->world();
    auto n  = w.driver().arg_nat("tensor", "vec-width");
    if (!n) return arg;
    if (std::ranges::find(Vec_widths, *n) == Vec_widths.end()) {
        w.log().w("ignoring `-X tensor:vec-width={}`: not one of {}", *n, fe::Join(Vec_widths, ", "));
        return arg;
    }
    return w.lit_nat(*n);
}

MIM_tensor_NORMALIZER_IMPL

} // namespace mim::plug::tensor
