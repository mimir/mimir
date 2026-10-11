#include "mim/phase/beta_red.h"

namespace mim {

static bool is_trivial(Lam* lam, const Var* var) {
    if (auto app = lam->body()->isa<App>()) {
        if (lam->ret_var() == app->callee() || !app->callee()->has_free_var(var))
            return std::ranges::all_of(app->targs(), [&](const Def* arg) {
                if (auto ex = arg->isa<Extract>(); ex && ex->tuple() == var) return true;
                return arg == var || !arg->has_free_var(var);
            });
    }
    return false;
}

static bool is_trivial(Lam* lam) {
    if (auto var = lam->has_var()) return is_trivial(lam, var);
    return true;
}

static Lam* forwardee(Lam* lam) {
    if (auto app = lam->body()->isa<App>())
        if (auto callee = app->callee()->isa_mut<Lam>(); callee && callee->is_set()) return callee;
    return nullptr;
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
        if (auto callee = forwardee(lam)) {
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
        return reduce(old_lam, rewrite(app->arg()));
    return RWPhase::rewrite_imm_App(app);
}

const Def* BetaRed::reduce(Lam* old_lam, const Def* new_arg) {
    profile_count("β-reduction");
    log().d("β-reduction `{}`", old_lam);
    invalidate();

    auto var = old_lam->has_var();
    if (!var) return rewrite(old_lam->body());

    if (is_once(old_lam)) {
        map(var, new_arg);
        return rewrite(old_lam->body());
    }

    // The old body may already be memoized for a rebuilt copy of old_lam, so substitute var directly.
    auto subst = [&](this auto&& self, const Def* def) -> const Def* {
        if (def == var) return new_arg;
        if (!def->has_free_var(var)) return rewrite(def);
        if (auto ex = def->isa<Extract>(); ex && ex->tuple() == var)
            return new_world().extract(new_arg, self(ex->index()));
        auto tuple = def->as<Tuple>();
        auto ops   = DefVec(tuple->ops(), [&](const Def* op) { return self(op); });
        return tuple->type()->has_free_var(var) ? new_world().tuple(ops)
                                                : new_world().tuple(rewrite(tuple->type()), ops);
    };

    auto app     = old_lam->body()->as<App>();
    auto fwd_arg = subst(app->arg());
    if (auto callee = app->callee()->isa_mut<Lam>(); callee && callee->is_set() && is_candidate(callee))
        return reduce(callee, fwd_arg);
    return new_world().app(subst(app->callee()), fwd_arg);
}

} // namespace mim
