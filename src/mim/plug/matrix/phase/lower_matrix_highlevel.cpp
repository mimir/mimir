#include "mim/plug/matrix/phase/lower_matrix_highlevel.h"

#include <mim/lam.h>

#include <mim/plug/cps/cps.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix::phase {

const Def* LowerMatrixHighLevelMapRed::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto& w = new_world();

    if (auto mat_ax = Axm::isa<matrix::prod>(app)) {
        auto [m, k, l] = mat_ax->decurry()->args<3>();
        auto pe        = mat_ax->decurry()->decurry()->arg();
        auto w_lit     = Lit::isa(pe);

        auto ext_fun = old_world().externals()[old_world().sym("extern_matrix_prod")];
        if (ext_fun && (w_lit && *w_lit == 64)) {
            auto [mem, M, N] = mat_ax->args<3>([this](const Def* def) { return rewrite(def); });
            auto ds_fun      = cps::op_cps2ds_dep(rewrite(ext_fun));
            return w.app(ds_fun, {mem, rewrite(m), rewrite(k), rewrite(l), M, N});
        }
    }

    // Maps a fully-applied high-level matrix axm to the `map_reduce_*` unfolding function that implements it.
    // clang-format off
    static const absl::flat_hash_map<flags_t, flags_t> axm_to_impl = {
        {flags_t(Annex::Base<prod>),      flags_t(Annex::Base<map_reduce_prod>)},
        {flags_t(Annex::Base<sum>),       flags_t(Annex::Base<map_reduce_sum>)},
        {flags_t(Annex::Base<transpose>), flags_t(Annex::Base<map_reduce_transpose>)},
    };
    // clang-format on

    if (auto axm = app->axm(); axm && app->curry() == 0) {
        if (auto i = axm_to_impl.find(axm->flags()); i != axm_to_impl.end()) {
            const Def* spec = w.annexes().flags2entry().at(i->second).def;
            // Re-apply every (fully-inferred) implicit meta group to the impl, outermost first. We apply them
            // explicitly (`w.app`, not `w.call`, which would insert fresh holes for the implicit domains and
            // misapply the groups), then cps2ds and hand over the operands.
            DefVec metas;
            for (const Def* c = app->callee(); auto ca = c->isa<App>(); c = ca->callee())
                metas.push_back(ca->arg());
            for (auto m = metas.rbegin(); m != metas.rend(); ++m)
                spec = w.app(spec, rewrite(*m));
            return w.call<cps::cps2ds>(spec, rewrite(app->arg()));
        }
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::matrix::phase
