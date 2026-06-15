#include "mim/plug/matrix/pass/lower_matrix_highlevel.h"

#include <mim/lam.h>

#include <mim/plug/direct/direct.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix {

namespace {

// clang-format off
absl::flat_hash_map<flags_t, flags_t> axm_to_impl_map = {
    {flags_t(Annex::Base<prod>),      flags_t(Annex::Base<mapRed_prod>)},
    {flags_t(Annex::Base<sum>),       flags_t(Annex::Base<mapRed_sum>)},
    {flags_t(Annex::Base<transpose>), flags_t(Annex::Base<mapRed_transpose>)},
};
// clang-format on

std::optional<const Def*> internal_function_of_axm(const Axm* axm, const Def* meta_args, const Def* args) {
    auto& world = axm->world();
    if (auto it = axm_to_impl_map.find(axm->flags()); it != axm_to_impl_map.end()) {
        const Def* spec_fun = world.implicit_app(world.annexes().flags2entry().at(it->second).def, meta_args);
        auto ds_fun         = direct::op_cps2ds_dep(spec_fun);
        return world.app(ds_fun, args);
    }
    return std::nullopt;
}

} // namespace

const Def* LowerMatrixHighLevelMapRed::rewrite(const Def* def) {
    if (auto i = rewritten.find(def); i != rewritten.end()) return i->second;
    auto new_def   = rewrite_(def);
    rewritten[def] = new_def;
    return rewritten[def];
}

const Def* LowerMatrixHighLevelMapRed::rewrite_(const Def* def) {
    if (auto mat_ax = Axm::isa<matrix::prod>(def)) {
        auto [mem, M, N]  = mat_ax->args<3>();
        auto [m, k, l, w] = mat_ax->decurry()->args<4>();
        auto w_lit        = Lit::isa(w);

        auto ext_fun = world().externals()[world().sym("extern_matrix_prod")];
        if (ext_fun && (w_lit && *w_lit == 64)) {
            auto ds_fun  = direct::op_cps2ds_dep(ext_fun);
            auto fun_app = world().app(ds_fun, {mem, m, k, l, M, N});
            return fun_app;
        }
    }

    if (auto outer_app = def->isa<App>()) {
        if (auto inner_app = outer_app->callee()->isa<App>()) {
            if (auto axm = inner_app->callee()->isa<Axm>()) {
                if (auto internal_function = internal_function_of_axm(axm, inner_app->arg(), outer_app->arg())) {
                    DLOG("lower matrix axm {} in {} : {}", *axm->sym(), def, def->type());
                    DLOG("lower matrix axm using: {} : {}", *internal_function, (*internal_function)->type());
                    return *internal_function;
                }
            }
        }
    }

    return def;
}

} // namespace mim::plug::matrix
