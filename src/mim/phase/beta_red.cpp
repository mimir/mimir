#include "mim/phase/beta_red.h"

namespace mim {

bool BetaRed::analyze() {
    for (auto def : world().roots())
        visit(def, false);
    return false; // no fixed-point neccessary
}

void BetaRed::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    for (auto d : def->deps())
        visit(d, true);
}

void BetaRed::visit(const Def* def, bool candidate) {
    if (auto lam = def->isa_mut<Lam>()) {
        if (auto [i, ins] = candidates_.emplace(lam, candidate); !ins) i->second = false;
    }
    analyze(def);
}

const Def* BetaRed::rewrite_imm_App(const App* app) {
    if (auto old_lam = app->callee()->isa_mut<Lam>(); old_lam && old_lam->is_set() && is_candidate(old_lam)) {
        profile_count("β-reduction");
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
