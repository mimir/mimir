#include "mim/phase/beta_red.h"

namespace mim {

bool BetaRed::analyze() {
    for (auto def : old_world().roots())
        visit(def, false);
    return false; // no fixed-point nccessary
}

void BetaRed::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) {
        // Back edge: this shared occurrence lies within the Lam%s it calls, so inlining it makes no progress.
        if (on_stack_.contains(def))
            for (auto mut : def->local_muts())
                if (auto lam = mut->isa<Lam>()) candidates_[lam] = false;
        return;
    }

    on_stack_.emplace(def);
    for (auto d : def->deps())
        visit(d, true);
    on_stack_.erase(def);
}

void BetaRed::visit(const Def* def, bool candidate) {
    if (auto lam = def->isa_mut<Lam>()) {
        if (auto [i, ins] = candidates_.emplace(lam, candidate); !ins) i->second = false;
    }
    analyze(def);
}

const Def* BetaRed::rewrite_imm_App(const App* app) {
    if (auto old_lam = app->callee()->isa_mut<Lam>();
        old_lam && old_lam->is_set() && is_candidate(old_lam) && !inlining_.contains(old_lam)) {
        profile_count("β-reduction");
        log().d("β-reduction `{}`", old_lam);
        if (auto var = old_lam->has_var()) {
            auto new_arg = rewrite(app->arg());
            map(var, new_arg);
            // if we want to reduce more than once, we need to push/pop
        }
        invalidate();
        inlining_.emplace(old_lam);
        auto res = rewrite(old_lam->body());
        inlining_.erase(old_lam);
        return res;
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim
