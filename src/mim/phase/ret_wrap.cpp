#include "mim/phase/ret_wrap.h"

namespace mim {

bool RetWrap::analyze() {
    for (auto def : old_world().annexes())
        visit(def);
    for (auto def : old_world().externals().muts())
        visit(def);

    return false; // no fixed-point neccessary
}

void RetWrap::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    if (auto app = def->isa<App>()) {
        visit(app->type());
        visit(app->callee(), Lattice::Single);
        visit(app->arg());
    } else {
        for (auto d : def->deps())
            visit(d);
    }
}

void RetWrap::visit(const Def* def, Lattice l) {
    if (auto lam = def->isa_mut<Lam>()) {
        if (auto ret_var = lam->ret_var()) {
            auto var = lam->has_var(); // must be set due to lam->ret_var() above
            join(ret_var, Bot);
            if (ret_var != lam->var()) {
                DLOG("{} → {}", var, ret_var);
                var2def_[var] = ret_var;
            }
        }
    } else if (auto i = def2lattice_.find(def); i != def2lattice_.end()) {
        i->second = join(i->second, l);
    } else if (auto app = def->isa<App>()) {
        if (auto var = app->arg()->isa<Var>()) {
            if (auto i = var2def_.find(var); i != var2def_.end()) {
                auto lam = var->mut()->as_mut<Lam>();
                DLOG("split: `{}`", lam);
                split_.emplace(lam);
                def2lattice_[i->second] = Eta;
            }
        }
    }

    analyze(def);
}

const Def* RetWrap::rewrite(const Def* old_def) {
    if (lattice(old_def) == Eta) {
        auto [i, ins] = def2eta_.emplace(old_def, nullptr);
        if (ins) i->second = Lam::eta_expand(rewrite_no_eta(old_def));
        DLOG("eta-expand: `{}` → `{}`", old_def, i->second);
        return i->second;
    }
    return RWPhase::rewrite(old_def);
}

const Def* RetWrap::rewrite_mut_Lam(Lam* old_lam) {
    if (split_.contains(old_lam)) {
        // rebuild a new "var" that substitutes the actual ret_var with ret_cont
        auto split_vars = old_lam->vars();
        auto ret_var    = old_lam->ret_var();
        assert(split_vars.back() == ret_var && "we assume that the last element is the ret_var");

        auto ret_cont     = Lam::eta_expand(ret_var);
        split_vars.back() = ret_cont;
        auto new_lam      = new_world().mut_lam(rewrite(old_lam->type())->as<Pi>());
        map(old_lam, new_lam);
        map(old_lam->var(), rewrite(old_world().tuple(split_vars)));
        return rewrite_stub(old_lam, new_lam);
    }

    return RWPhase::rewrite_mut_Lam(old_lam);
}

} // namespace mim
