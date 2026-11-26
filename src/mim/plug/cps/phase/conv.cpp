#include "mim/plug/cps/phase/conv.h"

#include "mim/plug/cps/cps.h"

namespace mim::plug::cps {

/// A term-level, direct-style function that we can and want to convert.
/// Type-level and higher-order functions stay in direct style.
static bool convertible(Lam* lam) {
    if (!lam->is_set() || lam->is_external() || lam->is_annex() || Lam::isa_cn(lam)) return false;
    auto codom = lam->codom();
    return !codom->isa<Type>() && !codom->isa<Pi>();
}

const Def* Conv::rewrite_mut_Lam(Lam* old_lam) {
    return convertible(old_lam) ? convert(old_lam) : DS2CPS::rewrite_mut_Lam(old_lam);
}

const Def* Conv::convert(Lam* old_lam) {
    auto& w      = new_world();
    auto old_pi  = old_lam->type();
    auto new_dom = rewrite(old_pi->dom());

    Lam* new_lam;
    if (auto old_var = old_pi->has_var()) {
        // dependent codom: bind it against a fresh mutable Sigma [a: A, Cn B[old_var → a]]
        auto sigma = w.mut_sigma(2);
        sigma->set(0, new_dom); // the codom may project sigma->var(2, 0), which needs its type
        sigma->var();
        push({}, Vars(sigma->has_var()));
        map(old_var, sigma->var(2, 0));
        auto new_codom = rewrite(old_pi->codom());
        pop();
        sigma->set(1, w.cn(new_codom));
        new_lam = w.mut_con(sigma);
    } else {
        new_lam = w.mut_fun(new_dom, rewrite(old_pi->codom()));
    }
    new_lam->set(old_lam->dbg_key())->debug_suffix("_cps");

    auto [param, ret] = new_lam->vars<2>();
    map(old_lam->var(), param);
    auto wrapper = map(old_lam, op_cps2ds_dep(new_lam));

    log().d("convert {}: {} → {}: {}", old_lam, old_pi, new_lam, new_lam->type());

    auto scope      = Scope(*this, true);
    auto new_filter = rewrite(old_lam->filter());
    auto tail       = w.app(ret, rewrite(old_lam->body()));
    new_lam->set(new_filter, wire(scope.base(), tail));

    return wrapper;
}

const Def* Conv::rewrite_imm_App(const App* old_app) {
    auto new_arg    = rewrite(old_app->arg());
    auto new_callee = rewrite(old_app->callee());

    if (liftable())
        if (auto wrapped = Axm::isa<cps2ds_dep>(new_callee))
            return lift(wrapped->arg(), {new_arg}, rewrite(old_app->type()))->var();

    return new_world().app(new_callee, new_arg);
}

} // namespace mim::plug::cps
