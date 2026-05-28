#include "mim/plug/cps/phase/conv.h"

namespace mim::plug::cps {

const Def* Conv::rewrite_mut_Lam(Lam* lam) {
    if (!Lam::isa_cn(lam)) {
        auto dom                = rewrite(lam->dom());
        auto codom              = rewrite(lam->codom());
        auto con                = new_world().mut_fun(dom, codom)->set(lam->dbg());
        auto re auto [var, ret] = con->vars<2>();
    }

    return RWPhase::rewrite_mut_Lam(lam);
}

const Def* Conv::rewrite_imm_App(const App* app) { return RWPhase::rewrite_imm_App(app); }

} // namespace mim::plug::cps
