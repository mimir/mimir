#include "mim/plug/matrix/phase/lower_matrix_highlevel.h"

#include <mim/lam.h>

#include <mim/plug/cps/cps.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix::phase {

const Def* LowerMatrixHighLevelMapRed::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto& w = new_world();

    // Each high-level op is implemented by a `map_reduce_*` unfolding function of matching type.
    // We re-apply its (fully-inferred) implicit meta groups explicitly with `w.app` — `w.call` would insert fresh holes
    // for the implicit domains and misapply the groups — to strip the meta down to the bare CPS function, then hand it
    // to `cps2ds` together with the operands.
    if (auto prod = Axm::isa<matrix::prod>(app)) {
        auto pe   = rewrite(prod->decurry()->decurry()->arg()); // {pe}
        auto dims = rewrite(prod->decurry()->arg());            // {m, k, l}
        auto spec = w.app(w.app(w.annex<map_reduce_prod>(), pe), dims);
        return w.call<cps::cps2ds>(spec, rewrite(prod->arg()));
    } else if (auto sum = Axm::isa<matrix::sum>(app)) {
        auto pe   = rewrite(sum->decurry()->decurry()->arg()); // {pe}
        auto dims = rewrite(sum->decurry()->arg());            // {n, S}
        auto spec = w.app(w.app(w.annex<map_reduce_sum>(), pe), dims);
        return w.call<cps::cps2ds>(spec, rewrite(sum->arg()));
    } else if (auto transpose = Axm::isa<matrix::transpose>(app)) {
        auto T    = rewrite(transpose->decurry()->decurry()->arg()); // {T}
        auto s    = rewrite(transpose->decurry()->arg());            // {s}
        auto spec = w.app(w.app(w.annex<map_reduce_transpose>(), T), s);
        return w.call<cps::cps2ds>(spec, rewrite(transpose->arg()));
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::matrix::phase
