#include "mim/plug/clos/phase/lower_typed_clos_prep.h"

#include <mim/plug/mem/mem.h>

namespace mim::plug::clos {

namespace {

bool interesting_type(const Def* type, DefSet& visited) {
    if (type->isa_mut()) visited.insert(type);
    if (isa_clos_type(type)) return true;
    if (auto sigma = type->isa<Sigma>())
        return std::any_of(sigma->ops().begin(), sigma->ops().end(),
                           [&](auto d) { return !visited.contains(d) && interesting_type(d, visited); });
    if (auto arr = type->isa<Arr>()) return interesting_type(arr->body(), visited);
    return false;
}

bool interesting_type(const Def* def) {
    auto visited = DefSet();
    return interesting_type(def->type(), visited);
}

void split(DefSet& out, const Def* def, bool as_callee) {
    if (auto lam = def->isa<Lam>()) {
        out.insert(lam);
    } else if (auto [var, lam] = ca_isa_var<Lam>(def); var && lam) {
        if (var->type()->isa<Pi>() || interesting_type(var)) out.insert(var);
    } else if (auto c = isa_clos_lit(def, false)) {
        split(out, c.fnc(), as_callee);
    } else if (auto a = Axm::isa<attr>(def)) {
        split(out, a->arg(), as_callee);
    } else if (auto proj = def->isa<Extract>()) {
        split(out, proj->tuple(), as_callee);
    } else if (auto pack = def->isa<Pack>()) {
        split(out, pack->body(), as_callee);
    } else if (auto tuple = def->isa<Tuple>()) {
        for (auto op : tuple->ops())
            split(out, op, as_callee);
    } else if (as_callee) {
        out.insert(def);
    }
}

DefSet split(const Def* def, bool keep_others) {
    DefSet out;
    split(out, def, keep_others);
    return out;
}

} // namespace

bool LowerTypedClosPrep::set_esc(const Def* def) {
    auto changed = false;
    for (auto d : split(def, false)) {
        if (is_esc(d)) continue;
        DLOG("set esc: {}", d);
        esc_.emplace(d);
        changed = true;
    }
    return changed;
}

bool LowerTypedClosPrep::analyze() {
    auto changed = false;
    DefSet done;

    auto visit = [&](this auto&& visit, const Def* def) -> void {
        if (!done.emplace(def).second) return;

        if (auto c = isa_clos_lit(def, false)) {
            DLOG("closure ({}, {})", c.env(), c.fnc());
            if (!c.fnc_as_lam() || is_esc(c.fnc_as_lam()) || is_esc(c.env_var())) changed |= set_esc(c.env());
        } else if (auto store = Axm::isa<mem::store>(def)) {
            DLOG("store {}", store->arg(2));
            changed |= set_esc(store->arg(2));
        } else if (auto app = def->isa<App>(); app && Pi::isa_cn(app->callee_type())) {
            DLOG("app {}", def);
            auto callees = split(app->callee(), true);
            for (auto i = 0_u64; i < app->num_args(); i++) {
                if (!interesting_type(app->arg(i))) continue;
                if (std::any_of(callees.begin(), callees.end(), [&](const Def* callee) {
                        if (auto lam = callee->isa_mut<Lam>()) return is_esc(lam->var(i));
                        return true;
                    }))
                    changed |= set_esc(app->arg(i));
            }
        }

        if (auto mut = def->isa_mut()) {
            if (mut->is_set())
                for (auto op : mut->deps())
                    visit(op);
        } else {
            for (auto op : def->deps())
                visit(op);
        }
    };

    for (auto def : old_world().roots())
        visit(def);

    return changed;
}

const Def* LowerTypedClosPrep::rewrite_imm_Tuple(const Tuple* tuple) {
    if (!is_bootstrapping()) {
        if (auto closure = isa_clos_lit(tuple, false)) {
            auto fnc = closure.fnc();
            if (!Axm::isa<attr>(fnc)) {
                auto new_fnc = new_world().call(esc_.contains(fnc) ? attr::esc : attr::bottom, rewrite(fnc));
                return clos_pack(rewrite(closure.env()), new_fnc, rewrite(closure->type()));
            }
        }
    }
    return RWPhase::rewrite_imm_Tuple(tuple);
}

} // namespace mim::plug::clos
