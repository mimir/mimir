#include "mim/plug/core/be/mlir/lam_classifier.h"

#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/affine/autogen.h>
#include <mim/plug/mem/mem.h>
#include <mim/plug/tensor/autogen.h>
#include <mim/plug/tensor/tensor.h>

namespace mim::mlir_be {

const Def* LamClassifier::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return Analysis::rewrite_imm_App(app);

    // %affine.For
    //
    if (auto for_ax = Axm::isa<plug::affine::For>(app)) {
        auto [body, exit, _] = for_ax->uncurry_args<3>();
        if (auto lam = body->isa_mut<Lam>()) results_[lam] = LamKind::AffineForBody;
        if (auto lam = exit->isa_mut<Lam>()) results_[lam] = LamKind::AffineForExit;
    }
    // %tensor.map_reduce
    if (Axm::isa<plug::tensor::map_reduce>(app)) {
        // app->callee()           has arg = subs
        // app->callee()->callee() has args = (comb, zero)
        auto callee        = app->callee()->as<App>();
        auto comb_zero_app = callee->callee()->as<App>();
        auto [comb, zero]  = comb_zero_app->arg()->projs<2>();
        if (auto lam = comb->isa_mut<Lam>()) {
            results_[lam]         = LamKind::MapReduceBody;
            map_reduce_apps_[lam] = app;
        }
    }

    return Analysis::rewrite_imm_App(app);
}

const Def* LamClassifier::rewrite_mut_Lam(Lam* lam) {
    if (is_bootstrapping()) return Analysis::rewrite_mut_Lam(lam);

    if (!results_.contains(lam)) {
        if (is_ignored(lam))
            results_[lam] = LamKind::Ignored;
        else if (lam->is_external())
            results_[lam] = LamKind::Function;
        else if (lam->type()->ret_pi())
            results_[lam] = LamKind::Function;
        else if (is_cond_branch(lam))
            results_[lam] = LamKind::CondBranch;
        else
            results_[lam] = LamKind::JoinBlock;
    }
    return Analysis::rewrite_mut_Lam(lam);
}

bool LamClassifier::is_ignored(Lam* lam) const {
    for (auto var : lam->vars())
        if (!Axm::isa<plug::mem::M>(var->type())) return false;
    return true;
}

bool LamClassifier::is_cond_branch(Lam* lam) const {
    if (!lam->is_set()) return false;
    if (auto app = lam->body()->isa<App>()) return bool(Dispatch(app));
    return false;
}

} // namespace mim::mlir_be
