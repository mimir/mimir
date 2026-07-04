#include "mim/phase/eta_red.h"

namespace mim {

void EtaRed::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_no_eta(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

const Def* EtaRed::rewrite(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>()) {
        if (auto callee = lam->eta_reduce()) {
            DLOG("eta-reduce: `{}` -> `{}`", lam, callee);
            invalidate();
            return rewrite(callee);
        }
    }

    return RWPhase::rewrite(old_def);
}

const Def* EtaRed::rewrite_imm_Var(const Var* var) { return new_world().var(rewrite_no_eta(var->mut())->as_mut()); }

} // namespace mim
