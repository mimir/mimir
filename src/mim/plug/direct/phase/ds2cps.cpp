#include "mim/plug/direct/phase/ds2cps.h"

#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/direct/direct.h"

namespace mim::plug::direct {

const Def* DS2CPS::rewrite_imm_App(const App* app) {
    if (auto lam = app->callee()->isa_mut<Lam>()) {
        auto new_lam = rewrite_lam(lam);
        if (new_lam != lam) {
            DLOG("new lam: {} : {}", new_lam, new_lam->type());
            DLOG("arg: {} : {}", app->arg(), app->arg()->type());
            auto new_app = new_world().app(new_lam, rewrite(app->arg()));
            DLOG("new app: {} : {}", new_app, new_app->type());
            return new_app;
        }
    }

    return RWPhase::rewrite_imm_App(app);
}

/// This function generates the cps function `f_cps : Cn [a: A, Cn B]` for a ds function `f: [a : A] -> B`.
/// The translation is associated in the `rewritten_` map.
const Def* DS2CPS::rewrite_lam(Lam* lam) {
    if (auto i = rewritten_.find(lam); i != rewritten_.end()) return i->second;

    auto pi    = lam->type();
    auto dom   = pi->dom();
    auto codom = pi->codom();

    if (Lam::isa_cn(lam)) return rewrite(lam);   // only look at lambdas (ds not cps)
    if (codom->isa<Type>()) return rewrite(lam); // ignore ds on type level
    if (codom->isa<Pi>()) return rewrite(lam);   // ignore higher order function

    DLOG("rewrite DS function {} : {}", lam, lam->type());

    auto sigma    = new_world().mut_sigma(2);
    auto rw_codom = codom;

    // replace ds dom var with cps sigma var (cps dom)
    if (auto var = pi->has_var()) {
        push();
        map(var, sigma->var(2, 0));
        rw_codom = rewrite(codom);
        pop();
    }

    sigma->set(0, dom);
    sigma->set(1, new_world().cn(rw_codom));

    DLOG("original codom: {}", codom);
    DLOG("rewritten codom: {}", rw_codom);

    auto cps_lam = new_world().mut_con(sigma)->set(lam->dbg());
    cps_lam->debug_suffix("_cps");
    auto [filter, cps_body] = lam->reduce<2>(cps_lam->var(2, 0));

    // rewrite vars of new function
    // calls handled separately
    DLOG("body: {} : {}", lam->body(), lam->body()->type());
    DLOG("cps body: {} : {}", cps_body, cps_body->type());

    cps_lam->app(filter, cps_lam->vars().back(), cps_body);

    rewritten_[lam] = op_cps2ds_dep(cps_lam);
    DLOG("replace {} : {}", lam, lam->type());
    DLOG("with {} : {}", rewritten_[lam], rewritten_[lam]->type());

    return rewritten_[lam];
}

} // namespace mim::plug::direct
