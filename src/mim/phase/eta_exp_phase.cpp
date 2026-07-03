#include "mim/phase/eta_exp_phase.h"

namespace mim {

bool EtaExpPhase::analyze() {
    for (auto def : old_world().roots())
        visit(def, Lattice::Known);
    return false; // no fixed-point neccessary
}

void EtaExpPhase::analyze(const Def* def, Lattice l) {
    auto acc = l;
    if (auto [i, ins] = analyzed_.emplace(def, l); !ins) {
        acc = join(i->second, l);
        if (acc == i->second) return; // no new information - already analyzed with this multiplicity
        i->second = acc;              // re-descend so that deps see the strengthened multiplicity
    }
    if (def->isa<Var>()) return; // ignore Var's mut

    // An immutable def used more than once in unknown position shares each of its unknown-position deps
    // just as often; muts reset this multiplicity - using a mut twice doesn't duplicate its body.
    auto down = !def->isa_mut() && (acc == Unknown_N || acc == Both) ? Unknown_N : Unknown_1;

    if (auto app = def->isa<App>()) {
        visit(app->type(), down);
        visit(app->callee(), Lattice::Known);
        visit(app->arg(), down);
    } else {
        for (auto d : def->deps())
            visit(d, down);
    }
}

void EtaExpPhase::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) join(lam, l);
    analyze(def, l);
}

void EtaExpPhase::rewrite_annex(flags_t flags, Sym sym, const Def* def) {
    new_world().annexes().attach(flags, sym, rewrite_no_eta(def));
}

void EtaExpPhase::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_no_eta(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

const Def* EtaExpPhase::rewrite(const Def* old_def) {
    // Don't wrap a Lam that is itself an η-redex - wrapping a wrapper adds nothing and never converges.
    if (auto lam = old_def->isa<Lam>(); lam && eta_expand(lam) && !lam->eta_reduce()) {
        auto eta = Lam::eta_expand(rewrite_no_eta(lam));
        DLOG("eta-expand: `{}` → `{}`", lam, eta);
        return eta;
    }

    return RWPhase::rewrite(old_def);
}

const Def* EtaExpPhase::rewrite_imm_App(const App* app) {
    auto callee = rewrite_no_eta(app->callee());
    return new_world().app(callee, rewrite(app->arg()));
}

const Def* EtaExpPhase::rewrite_imm_Var(const Var* var) {
    return new_world().var(rewrite_no_eta(var->mut())->as_mut());
}

} // namespace mim
