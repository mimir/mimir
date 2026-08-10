#include "mim/plug/tensor/phase/lower.h"

#include <mim/def.h>
#include <mim/lam.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

const Def* Lower::lower_via_impl(const App* app, const Def* impl_annex) {
    auto& w = new_world();

    // Walk the curry chain (innermost App outermost in syntax) to collect the args
    // in the order they were applied.
    DefVec args;
    const Def* head = app;
    while (auto h = head->isa<App>()) {
        args.push_back(rewrite(h->arg()));
        head = h->callee();
    }
    std::reverse(args.begin(), args.end());

    auto impl = impl_annex;
    for (auto a : args)
        impl = w.app(impl, a);

    // The `_impl` is a `lam`, so applying it triggers beta-reduction. Each `_impl`
    // body references the `_impl` variants of its dependencies directly, so the
    // chain bottoms out at the low-level axioms (`map_reduce`, …) in one go.
    return impl;
}

const Def* Lower::rewrite_imm_App(const App* app) {
    auto& w = new_world();

    if (Axm::isa<tensor::broadcast_in_dim>(app))
        return lower_via_impl(app, w.annex<tensor::broadcast_in_dim_impl>());
    else if (Axm::isa<tensor::product_2d>(app))
        return lower_via_impl(app, w.annex<tensor::product_2d_impl>());
    else if (Axm::isa<tensor::bmm>(app))
        return lower_via_impl(app, w.annex<tensor::bmm_impl>());
    else if (Axm::isa<tensor::dot_product>(app))
        return lower_via_impl(app, w.annex<tensor::dot_product_impl>());
    else if (Axm::isa<tensor::transpose>(app))
        return lower_via_impl(app, w.annex<tensor::transpose_impl>());
    else if (Axm::isa<tensor::transpose_2d>(app))
        return lower_via_impl(app, w.annex<tensor::transpose_2d_impl>());
    else if (Axm::isa<tensor::map>(app))
        return lower_via_impl(app, w.annex<tensor::map_impl>());
    else if (Axm::isa<tensor::unary>(app))
        return lower_via_impl(app, w.annex<tensor::unary_impl>());
    else if (Axm::isa<tensor::binary>(app))
        return lower_via_impl(app, w.annex<tensor::binary_impl>());
    else if (Axm::isa<tensor::select>(app))
        return lower_via_impl(app, w.annex<tensor::select_impl>());
    else if (Axm::isa<tensor::repeat>(app))
        return lower_via_impl(app, w.annex<tensor::repeat_impl>());
    else if (Axm::isa<tensor::reshape>(app))
        return lower_via_impl(app, w.annex<tensor::reshape_impl>());
    else if (Axm::isa<tensor::slice>(app))
        return lower_via_impl(app, w.annex<tensor::slice_impl>());
    else if (Axm::isa<tensor::flip>(app))
        return lower_via_impl(app, w.annex<tensor::flip_impl>());
    else if (Axm::isa<tensor::conv>(app))
        return lower_via_impl(app, w.annex<tensor::conv_impl>());
    else if (Axm::isa<tensor::pool>(app))
        return lower_via_impl(app, w.annex<tensor::pool_impl>());
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
