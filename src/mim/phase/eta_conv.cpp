#include "mim/phase/eta_conv.h"

namespace mim {

bool EtaConv::analyze() {
    for (auto def : old_world().roots())
        visit(def, Lattice::Known);
    return false; // no fixed-point neccessary
}

void EtaConv::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;
    if (def->isa<Var>()) return; // ignore Var's mut

    if (auto app = def->isa<App>()) {
        visit(app->type(), Lattice::Unknown_1);
        visit(app->callee(), Lattice::Known);
        visit(app->arg(), Lattice::Unknown_1);
    } else {
        for (auto d : def->deps())
            visit(d, Lattice::Unknown_1);
    }
}

void EtaConv::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) {
        // Wrapper-transparency: a use of `λx.f x` counts as a use of `f` at the same lattice `l`.
        // Do *not* descend into the wrapper's body (which would classify `f` as Known).
        if (auto f = lam->eta_reduce()) return visit(f, l);
        join(lam, l);
    }
    analyze(def);
}

void EtaConv::rewrite_annex(flags_t flags, Sym sym, const Def* def) {
    new_world().annexes().attach(flags, sym, rewrite_no_eta(def));
}

void EtaConv::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_no_eta(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

const Def* EtaConv::rewrite(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>()) {
        if (auto f = lam->eta_reduce()) {
            // η-redex `λx.f x`: reduce unless `f` wants to stay expanded.
            if (!keep_wrapper(f)) {
                DLOG("eta-reduce: `{}` → `{}`", lam, f);
                invalidate();
                return rewrite(f);
            }
            // Keep it - but canonicalize as a fresh expansion wrapper (with the `tt` filter EtaExp uses), so a
            // pre-existing wrapper's stale filter does not leak downstream.
            return Lam::eta_expand(rewrite_no_eta(f));
        } else if (eta_expand(lam)) {
            // bare Lam used in an unknown position more than once or in both positions: η-expand.
            auto eta = Lam::eta_expand(rewrite_no_eta(lam));
            DLOG("eta-expand: `{}` → `{}`", lam, eta);
            invalidate();
            return eta;
        }
    }

    return RWPhase::rewrite(old_def);
}

const Def* EtaConv::rewrite_no_exp(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>())
        if (auto f = lam->eta_reduce(); f && !keep_wrapper(f)) {
            DLOG("eta-reduce: `{}` → `{}`", lam, f);
            invalidate();
            return rewrite_no_exp(f);
        }
    return RWPhase::rewrite(old_def);
}

const Def* EtaConv::rewrite_imm_App(const App* app) {
    auto callee = rewrite_no_exp(app->callee());
    return new_world().app(callee, rewrite(app->arg()));
}

const Def* EtaConv::rewrite_imm_Var(const Var* var) { return new_world().var(rewrite_no_eta(var->mut())->as_mut()); }

} // namespace mim
