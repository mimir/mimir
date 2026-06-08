#include "mim/phase/eta_exp_phase.h"

namespace mim {

bool EtaExpPhase::analyze() {
    for (auto def : old_world().roots())
        visit(def, Lattice::Known);
    return false; // no fixed-point neccessary
}

void EtaExpPhase::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;
    if (def->isa<Var>()) return; // ignore Var's mut

    if (auto app = def->isa<App>()) {
        visit(app->type(), Lattice::Unknown);
        visit(app->callee(), Lattice::Known);
        visit(app->arg(), Lattice::Unknown);
    } else {
        for (auto d : def->deps())
            visit(d, Lattice::Unknown);
    }
}

void EtaExpPhase::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) join(lam, l);
    analyze(def);
}

void EtaExpPhase::rewrite_annex(flags_t f, const Def* def) { new_world().register_annex(f, rewrite_no_eta(def)); }

void EtaExpPhase::rewrite_external(Def* old_mut) {
    auto new_mut = rewrite_no_eta(old_mut)->as_mut();
    if (old_mut->is_external()) new_mut->externalize();
}

const Def* EtaExpPhase::rewrite(const Def* old_def) {
    if (auto lam = old_def->isa<Lam>(); lam && lattice(lam) == Both) {
        auto [i, ins] = lam2eta_.emplace(lam, nullptr);
        if (ins) i->second = Lam::eta_expand(rewrite_no_eta(lam));
        DLOG("eta-expand: `{}` → `{}`", lam, i->second);
        return i->second;
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
