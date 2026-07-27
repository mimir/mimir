#include "mim/plug/matrix/phase/lower_matrix_highlevel.h"

#include <mim/lam.h>

#include <mim/plug/cps/cps.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix::phase {

namespace {

/// Maps a high-level matrix axm to the `map_reduce_*` unfolding function that implements it.
std::optional<const Def*>
internal_function_of_axm(World& world, const Axm* axm, const Def* meta_args, const Def* args) {
    // clang-format off
    static const absl::flat_hash_map<flags_t, flags_t> axm_to_impl = {
        {flags_t(Annex::Base<prod>),      flags_t(Annex::Base<map_reduce_prod>)},
        {flags_t(Annex::Base<sum>),       flags_t(Annex::Base<map_reduce_sum>)},
        {flags_t(Annex::Base<transpose>), flags_t(Annex::Base<map_reduce_transpose>)},
    };
    // clang-format on
    if (auto it = axm_to_impl.find(axm->flags()); it != axm_to_impl.end()) {
        auto spec_fun = world.implicit_app(world.annexes().flags2entry().at(it->second).def, meta_args);
        return world.app(cps::op_cps2ds_dep(spec_fun), args);
    }
    return std::nullopt;
}

} // namespace

const Def* LowerMatrixHighLevelMapRed::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto& w = new_world();

    if (auto mat_ax = Axm::isa<matrix::prod>(app)) {
        auto [m, k, l, width] = mat_ax->decurry()->args<4>();
        auto w_lit            = Lit::isa(width);

        auto ext_fun = old_world().externals()[old_world().sym("extern_matrix_prod")];
        if (ext_fun && (w_lit && *w_lit == 64)) {
            auto [mem, M, N] = mat_ax->args<3>([this](const Def* def) { return rewrite(def); });
            auto ds_fun      = cps::op_cps2ds_dep(rewrite(ext_fun));
            return w.app(ds_fun, {mem, rewrite(m), rewrite(k), rewrite(l), M, N});
        }
    }

    if (auto inner_app = app->callee()->isa<App>()) {
        if (auto axm = inner_app->callee()->isa<Axm>()) {
            auto new_meta_args = rewrite(inner_app->arg());
            auto new_args      = rewrite(app->arg());
            if (auto internal_function = internal_function_of_axm(w, axm, new_meta_args, new_args))
                return *internal_function;
        }
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::matrix::phase
