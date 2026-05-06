#include "mim/phase/beta_red_phase.h"

namespace mim {

bool BetaRedPhase::analyze() {
    for (auto def : old_world().annexes())
        visit(def, false);
    for (auto def : old_world().externals().muts())
        visit(def, false);

    return false; // no fixed-point neccessary
}

void BetaRedPhase::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;
    if (def->isa<Var>()) return; // ignore Var's mut

    for (auto d : def->deps())
        visit(d, true);
}

void BetaRedPhase::visit(const Def* def, bool candidate) {
    if (auto lam = def->isa_mut<Lam>()) {
        if (auto [i, ins] = candidates_.emplace(lam, candidate); !ins) i->second = false;
    }
    analyze(def);
}

const Def* BetaRedPhase::rewrite_imm_App(const App* app) {
    if (auto old_lam = app->callee()->isa_mut<Lam>(); old_lam && old_lam->is_set() && is_candidate(old_lam)) {
        DLOG("beta-reduce: `{}`", old_lam);
        if (auto var = old_lam->has_var()) {
            auto new_arg = rewrite(app->arg());
            map(var, new_arg);
            // if we want to reduce more than once, we need to push/pop
        }
        invalidate();
        return rewrite(old_lam->body());
    }

    return Rewriter::rewrite_imm_App(app);
}

} // namespace mim
