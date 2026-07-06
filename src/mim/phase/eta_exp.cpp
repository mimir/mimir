#include "mim/phase/eta_exp.h"

namespace mim {

bool EtaExp::analyze() {
    for (auto def : old_world().roots())
        visit(def, Lattice::Known);
    return false; // no fixed-point neccessary
}

void EtaExp::analyze(const Def* def) {
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

void EtaExp::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) join(lam, l);
    analyze(def);
}

void EtaExp::rewrite_annex(flags_t flags, Sym sym, const Def* def) {
    new_world().annexes().attach(flags, sym, rewrite_no_eta(def));
}

void EtaExp::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_no_eta(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

const Def* EtaExp::rewrite(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>(); lam && eta_expand(lam)) {
        auto eta = Lam::eta_expand(rewrite_no_eta(lam));
        DLOG("eta-expand: `{}` → `{}`", lam, eta);
        return eta;
    }

    return RWPhase::rewrite(old_def);
}

const Def* EtaExp::rewrite_imm_App(const App* app) {
    auto callee = rewrite_no_eta(app->callee());
    return new_world().app(callee, rewrite(app->arg()));
}

const Def* EtaExp::rewrite_imm_Var(const Var* var) { return new_world().var(rewrite_no_eta(var->mut())->as_mut()); }

} // namespace mim
