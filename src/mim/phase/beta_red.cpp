#include "mim/phase/beta_red.h"

namespace mim {

static bool is_trivial(Lam* lam) {
    if (auto var = lam->has_var()) {
        if (auto app = lam->body()->isa<App>()) {
            if (lam->ret_var() == app->callee() || !app->callee()->has_free_var(var))
                return std::ranges::all_of(app->targs(), [&](const Def* arg) {
                    if (auto ex = arg->isa<Extract>(); ex && ex->tuple() == var) return true;
                    return arg == var || !arg->has_free_var(var);
                });
        }
        return false;
    }
    return true;
}

bool BetaRed::analyze() {
    for (auto def : old_world().externals().muts())
        visit(def, false);

    // Inlining a trivial Lam at several call sites duplicates its callee's occurrence.
    auto worklist = std::deque<Lam*>();
    for (auto [lam, once] : once_)
        if (!once && trivial_.contains(lam) && !cyclic_.contains(lam)) worklist.emplace_back(lam);

    while (!worklist.empty()) {
        auto lam = worklist.front();
        worklist.pop_front();
        if (auto callee = lam->forwardee()) {
            if (auto& once = once_[callee]; once) {
                once = false;
                if (trivial_.contains(callee) && !cyclic_.contains(callee)) worklist.emplace_back(callee);
            }
        }
    }

    return false; // no fixed-point nccessary
}

void BetaRed::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) {
        // Back edge: this shared occurrence lies within the Lam%s it calls, so inlining it makes no progress.
        if (on_stack_.contains(def))
            for (auto mut : def->local_muts())
                if (auto lam = mut->isa<Lam>()) cyclic_.emplace(lam);
        return;
    }

    on_stack_.emplace(def);
    for (auto d : def->deps())
        visit(d, true);
    on_stack_.erase(def);
}

void BetaRed::visit(const Def* def, bool candidate) {
    if (auto lam = def->isa_mut<Lam>()) {
        if (auto [i, ins] = once_.emplace(lam, candidate); !ins)
            i->second = false;
        else if (lam->is_set() && is_trivial(lam))
            trivial_.emplace(lam);
    }
    analyze(def);
}

const Def* BetaRed::rewrite_imm_App(const App* app) {
    if (auto old_lam = app->callee()->isa_mut<Lam>(); old_lam && old_lam->is_set() && is_candidate(old_lam))
        return reduce(old_lam, app->arg());
    return RWPhase::rewrite_imm_App(app);
}

const Def* BetaRed::reduce(Lam* old_lam, const Def* old_arg) {
    profile_count("β-reduction");
    log().d("β-reduction `{}`", old_lam);
    invalidate();

    if (auto var = old_lam->has_var(); var && is_once(old_lam)) {
        map(var, rewrite(old_arg));
        return rewrite(old_lam->body());
    }

    // The old body may already be memoized for a rebuilt copy of old_lam, so reduce in the old world.
    return rewrite(old_lam->reduce_body(old_arg));
}

} // namespace mim
