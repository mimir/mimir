#include "mim/plug/matrix/phase/lower.h"

#include <mim/lam.h>

#include <mim/plug/cps/cps.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix::phase {

const Def* Lower::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto& w = new_world();

    // Each high-level op is implemented by an `*_impl` annex of matching type.
    // `%matrix.product_2d` / `%matrix.sum` lead with an explicit `[R: %matrix.Ring]` group and
    // `%matrix.transpose_2d` with an implicit `{T}`; either way the curry shape is the same, so the two leading
    // groups are re-applied positionally.
    // We re-apply its (fully-inferred) implicit meta groups explicitly with `w.app` — `w.call` would insert fresh holes
    // for the implicit domains and misapply the groups — to strip the meta down to the bare CPS function, then hand it
    // to `cps2ds` together with the operands.
    if (auto product_2d = Axm::isa<matrix::product_2d>(app)) {
        auto R    = rewrite(product_2d->decurry()->decurry()->arg()); // [R]
        auto dims = rewrite(product_2d->decurry()->arg());            // {m, k, l}
        auto spec = w.app(w.app(w.annex<product_2d_impl>(), R), dims);
        return w.call<cps::cps2ds>(spec, rewrite(product_2d->arg()));
    } else if (auto sum = Axm::isa<matrix::sum>(app)) {
        auto R    = rewrite(sum->decurry()->decurry()->arg()); // [R]
        auto dims = rewrite(sum->decurry()->arg());            // {n, S}
        auto spec = w.app(w.app(w.annex<sum_impl>(), R), dims);
        return w.call<cps::cps2ds>(spec, rewrite(sum->arg()));
    } else if (auto transpose_2d = Axm::isa<matrix::transpose_2d>(app)) {
        auto T    = rewrite(transpose_2d->decurry()->decurry()->arg()); // {T}
        auto s    = rewrite(transpose_2d->decurry()->arg());            // {s}
        auto spec = w.app(w.app(w.annex<transpose_2d_impl>(), T), s);
        return w.call<cps::cps2ds>(spec, rewrite(transpose_2d->arg()));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::matrix::phase
