#include "mim/phase/eta_conv.h"

namespace mim {

bool EtaConv::analyze() {
    for (auto def : old_world().externals().muts())
        visit(def, Lattice::Known);
    return false; // no fixed-point necessary
}

// Such an immutable is unfolded into a tree up to the next App or mutable, so each occurrence gets its own wrappers.
static bool is_incarnated(const Def* def) {
    return !def->isa_mut() && !def->isa<App>() && !def->isa<Var>() && !def->local_muts().empty();
}

void EtaConv::analyze(const Def* def) {
    if (def->local_muts().empty()) return;
    if (!is_incarnated(def) && !analyzed_.emplace(def).second) return;

    if (auto app = def->isa<App>()) return analyze_app(app);
    auto lam = def->isa_mut<Lam>();
    for (auto d : def->deps())
        if (auto app = lam && d == lam->body() ? d->isa<App>() : nullptr)
            analyze_app(app);
        else
            visit(d, Lattice::Unknown_1);
}

void EtaConv::analyze_app(const App* app) {
    // clang-format off
    visit(app->type(),   Lattice::Unknown_1);
    visit(app->callee(), Lattice::Known);
    visit(app->arg(),    Lattice::Unknown_1);
    // clang-format on
}

void EtaConv::visit(const Def* def, Lattice l) {
    // Wrapper-transparency: a use of `λx.f x` counts as a use of `f` at the same lattice `l`.
    // Do *not* descend into the wrapper's body (which would classify `f` as Known).
    if (auto f = Lam::eta_reduce(def)) return visit(f, l);
    if (auto lam = def->isa_mut<Lam>()) join(lam, l);
    analyze(def);
}

const Def* EtaConv::lookup(const Def* old_def) {
    if (is_incarnated(old_def) && !old_world().annexes().opaque(old_def)) return nullptr;
    return RWPhase::lookup(old_def);
}

const Def* EtaConv::rewrite(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>()) {
        if (auto f = Lam::eta_reduce(lam)) {
            // η-redex `λx.f x`: reduce unless `f` wants to stay expanded.
            if (!expand(f)) return rewrite_no_exp(lam);
            // Keep it - but as a *fresh* expansion wrapper (with a `tt` filter), so that a
            // pre-existing wrapper's stale filter does not leak downstream and every occurrence gets its own.
            return Lam::eta_expand(rewrite_no_eta(f));
        } else if (expand(lam)) {
            // bare Lam used in an unknown position more than once or in both positions: η-expand.
            profile_count("η-expansion");
            auto eta = Lam::eta_expand(rewrite_no_eta(lam));
            log().d("eta-expand {} → {}", lam, eta);
            invalidate();
            return eta;
        }
    }

    return RWPhase::rewrite(old_def);
}

// Two Lams with the same body App must not share its wrappers, or the wrappers become critical edges.
std::pair<const Def*, const Def*> EtaConv::rewrite_ops(Lam* old_lam, Lam*) {
    auto body     = old_lam->body();
    auto new_body = body->isa<App>() ? rewrite_imm(body)->set(body->dbg_key()) : rewrite(body);
    return {rewrite(old_lam->filter()), new_body};
}

const Def* EtaConv::rewrite_no_exp(const Def* old_def) {
    if (auto f = Lam::eta_reduce(old_def); f && !expand(f)) {
        profile_count("η-reduction");
        log().d("eta-reduce {} → {}", old_def, f);
        invalidate();
        return rewrite_no_eta(f);
    }
    return rewrite_no_eta(old_def);
}

const Def* EtaConv::rewrite_imm_App(const App* app) {
    auto callee = rewrite_no_exp(app->callee());
    return new_world().app(callee, rewrite(app->arg()));
}

const Def* EtaConv::rewrite_imm_Var(const Var* var) { return new_world().var(rewrite_no_eta(var->binder())->as_mut()); }

} // namespace mim
