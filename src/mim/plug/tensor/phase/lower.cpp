#include "mim/plug/tensor/phase/lower.h"

#include <ranges>

#include <mim/def.h>
#include <mim/lam.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

/// One step down an `Extract` chain over array types; `nullptr` at the base. A Sigma projection ends the chain.
const Extract* array_extract(const Def* d) {
    auto ex = d->isa<Extract>();
    if (!ex || !ex->tuple()->type()->isa<Arr>()) return nullptr;
    auto ty = ex->index()->unfold_type();
    return Idx::isa(ty) || Lit::isa(ty->arity()) ? ex : nullptr;
}

const Def* base_of(const Def* d) {
    while (auto ex = array_extract(d))
        d = ex->tuple();
    return d;
}

/// The chain's index components, one per axis and outermost first - a fused index contributes all of its own.
DefVec axes_of(const Def* d) {
    auto axes = DefVec(); // collected innermost-axis-first and reversed once at the end
    for (auto ex = array_extract(d); ex; ex = array_extract(d)) {
        auto idx = ex->index();
        if (auto r = Lit::isa(idx->unfold_type()->arity()); r && !Idx::isa(idx->unfold_type()))
            for (nat_t i = *r; i-- != 0;)
                axes.emplace_back(idx->proj(*r, i));
        else
            axes.emplace_back(idx);
        d = ex->tuple();
    }
    std::ranges::reverse(axes);
    return axes;
}

} // namespace

const Def* Lower::read_through(const Def* base, Defs index) {
    auto& w = new_world();

    const Def* input;
    const Def* s_in;
    const Def* s_out;
    const Def* rank;
    bool wraps; // a repeat reads `idx mod s_in` per axis, a broadcast only ever reads a size-1 axis at 0

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

    auto r = Lit::isa(rank);
    if (!r) return nullptr;

    auto new_index = DefVec(*r);
    for (u64 d = 0, i = 0; d != *r; ++d) {
        auto in_d  = s_in->proj(*r, d);
        auto out_d = s_out->proj(*r, d);
        // A size-1 output axis folds out of the array type, so the chain carries no index for it.
        auto idx_d = Lit::isa(out_d) == 1 ? w.lit_idx(1, 0) : (i < index.size() ? rewrite(index[i++]) : nullptr);
        if (!idx_d) return nullptr;

        if (in_d == out_d)
            new_index[d] = idx_d;
        else if (Lit::isa(in_d) == 1)
            new_index[d] = w.lit_idx(1, 0);
        else if (auto e = Lit::isa(in_d), x = Lit::isa(idx_d); wraps && e && x)
            new_index[d] = w.lit_idx(*e, *x % *e);
        else
            return nullptr;
    }

    return w.extract(rewrite(input), w.tuple(new_index));
}

const Def* Lower::rewrite_imm_Extract(const Extract* extract) {
    // Only a repeat/broadcast base reads through, so find it before collecting any indices.
    if (auto base = base_of(extract); Axm::isa<tensor::repeat>(base) || Axm::isa<tensor::broadcast>(base))
        if (auto res = read_through(base, axes_of(extract))) return res;
    return RWPhase::rewrite_imm_Extract(extract);
}

const Def* Lower::fastest_axis_2(const App* app, const Def* rank) {
    auto& w = new_world();
    auto b  = rewrite(app->arg()->proj(2, 1));
    return w.app(w.app(w.annex<tensor::fastest_axis>(), b->type()), {rank, b});
}

const Def* Lower::lower_via_impl(const App* app, const Def* impl_annex) {
    auto& w = new_world();

    // The curry chain, innermost App first — hence re-applied in reverse.
    auto args = DefVec();
    for (const App* h = app; h; h = h->callee()->isa<App>())
        args.emplace_back(rewrite(h->arg()));

    // The `_impl` is a `lam`, so applying it triggers beta-reduction. Each `_impl`
    // body references the `_impl` variants of its dependencies directly, so the
    // chain bottoms out at the low-level axioms (`map_reduce`, …) in one go.
    auto impl = impl_annex;
    for (auto a : args | std::views::reverse)
        impl = w.app(impl, a);
    return impl;
}

const Def* Lower::rewrite_imm_App(const App* app) {
    auto& w = new_world();

    if (Axm::isa<tensor::broadcast_in_dim>(app)) return lower_via_impl(app, w.annex<tensor::broadcast_in_dim_impl>());
    if (Axm::isa<tensor::transpose>(app)) return lower_via_impl(app, w.annex<tensor::transpose_impl>());
    if (Axm::isa<tensor::transpose_2d>(app)) return lower_via_impl(app, w.annex<tensor::transpose_2d_impl>());
    if (Axm::isa<tensor::map>(app)) return lower_via_impl(app, w.annex<tensor::map_impl>());
    if (Axm::isa<tensor::unary>(app)) return lower_via_impl(app, w.annex<tensor::unary_impl>());
    if (Axm::isa<tensor::binary>(app)) return lower_via_impl(app, w.annex<tensor::binary_impl>());
    if (Axm::isa<tensor::select>(app)) return lower_via_impl(app, w.annex<tensor::select_impl>());
    if (Axm::isa<tensor::repeat>(app)) return lower_via_impl(app, w.annex<tensor::repeat_impl>());
    if (Axm::isa<tensor::reshape>(app)) return lower_via_impl(app, w.annex<tensor::reshape_impl>());
    if (Axm::isa<tensor::slice>(app)) return lower_via_impl(app, w.annex<tensor::slice_impl>());
    if (Axm::isa<tensor::flip>(app)) return lower_via_impl(app, w.annex<tensor::flip_impl>());
    if (Axm::isa<tensor::conv>(app)) return lower_via_impl(app, w.annex<tensor::conv_impl>());
    if (Axm::isa<tensor::pool>(app)) return lower_via_impl(app, w.annex<tensor::pool_impl>());

    // The dot family's `_impl`s take a leading `fastest_2` with no axiom counterpart — the
    // `tensor.fastest_axis` reflection of the right operand, pre-applied here at the staging
    // point where that operand is concrete (see tensor.dot_product_impl for the decision).
    if (Axm::isa<tensor::product_2d>(app))
        return lower_via_impl(app, w.app(w.annex<tensor::product_2d_impl>(), fastest_axis_2(app, w.lit_nat(2))));
    if (Axm::isa<tensor::bmm>(app))
        return lower_via_impl(app, w.app(w.annex<tensor::bmm_impl>(), fastest_axis_2(app, w.lit_nat(3))));
    if (Axm::isa<tensor::dot_product>(app)) {
        // The curry chain, outermost app first: [a, b] {s1 s2} [c1, c2, b1, b2] {nc nb} {r1 r2};
        // the right operand's rank is {r1 r2}#1.
        auto groups = app->callee()->as<App>()->callee()->as<App>()->callee()->as<App>();
        auto r2     = groups->callee()->as<App>()->arg()->proj(2, 1);
        return lower_via_impl(app, w.app(w.annex<tensor::dot_product_impl>(), fastest_axis_2(app, rewrite(r2))));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
