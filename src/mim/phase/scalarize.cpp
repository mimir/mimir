#include "mim/phase/scalarize.h"

#include "mim/tuple.h"

namespace mim {

// TODO should also work for mutable non-dependent sigmas

bool Scalarize::expandable_type(Lam* lam) {
    auto pi = lam->type();
    if (!(lam->num_doms() > 1 && Pi::isa_cn(pi) && pi->isa_imm())) return false; // no ugly dependent pis

    auto types = DefVec();
    bool todo  = false;
    for (size_t i = 0, e = lam->num_doms(); i != e; ++i) {
        auto n = flatten(types, lam->dom(i), false);
        todo |= n != 1 || types.back() != lam->dom(i);
    }
    return todo;
}

void Scalarize::analyze_uses() {
    uses_analyzed_ = true;

    // A Lam may appear as an App's callee, or as op of a branch tuple that is only extracted as callee.
    // Any other position makes it escape - as do branch tuples that mix Lams of different types
    // (all sides of a branch must keep identical types after expansion).
    auto is_lam_tuple = [](const Def* def) {
        auto tuple = def->isa<Tuple>();
        return tuple && tuple->num_ops() != 0
            && std::all_of(tuple->ops().begin(), tuple->ops().end(), [](const Def* op) { return op->isa_mut<Lam>(); });
    };
    auto escape_all = [&](const Def* def) {
        if (auto lam = def->isa_mut<Lam>()) escaped_.emplace(lam);
        if (auto tuple = def->isa<Tuple>())
            for (auto op : tuple->ops())
                if (auto lam = op->isa_mut<Lam>()) escaped_.emplace(lam);
    };

    DefSet done;
    auto visit = [&](auto&& visit, const Def* def) -> void {
        if (!done.emplace(def).second) return;
        if (def->isa<Var>()) return; // a Var references its mut as op; that is not a use of the Lam

        for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
            auto op = def->op(i);
            if (!op) continue;
            auto callee_pos = def->isa<App>() && i == 0;

            if (auto lam = op->isa_mut<Lam>()) {
                // ops of a lam tuple are judged when the tuple itself is used
                if (!callee_pos && !is_lam_tuple(def)) escaped_.emplace(lam);
            } else if (is_lam_tuple(op)) {
                // all sides of a branch share one expansion decision:
                // if one cannot be expanded, none may be, or the tuple would become heterogeneous
                auto extract_pos = def->isa<Extract>() && i == 0;
                auto expandable  = std::all_of(op->ops().begin(), op->ops().end(), [&](const Def* l) {
                    return l->type() == op->op(0)->type() && isa_optimizable(l->as_mut<Lam>());
                });
                if (!extract_pos || !expandable) escape_all(op);
            } else if (auto proj = op->isa<Extract>(); proj && is_lam_tuple(proj->tuple())) {
                if (!callee_pos) escape_all(proj->tuple());
            }

            visit(visit, op);
        }

        if (auto mut = def->isa_mut(); mut && mut->is_set())
            for (auto op : mut->deps())
                visit(visit, op);
    };

    for (auto def : old_world().roots())
        visit(visit, def);
}

bool Scalarize::should_expand(Lam* lam) {
    if (!isa_optimizable(lam)) return false;
    if (auto i = decided_.find(lam); i != decided_.end()) return i->second;
    if (!uses_analyzed_) analyze_uses();

    auto ok = !escaped_.contains(lam) && expandable_type(lam);
    DLOG("should_expand({}) = {}", lam, ok);
    return decided_[lam] = ok;
}

const Def* Scalarize::rewrite_mut_Lam(Lam* old) {
    if (!is_bootstrapping() && should_expand(old)) {
        auto& w     = new_world();
        auto types  = DefVec();
        auto arg_sz = Vector<size_t>();
        for (size_t i = 0, e = old->num_doms(); i != e; ++i)
            arg_sz.push_back(flatten(types, rewrite(old->dom(i)), false));

        auto sca_lam = w.mut_lam(w.cn(types))->set(old->dbg());
        DLOG("lambda {} : {} ~> {} : {}", old, old->type(), sca_lam, sca_lam->type());
        map(old, sca_lam);

        size_t n      = 0;
        auto new_vars = w.tuple(DefVec(old->num_doms(), [&](size_t i) {
            auto tuple = DefVec(arg_sz.at(i), [&](auto) { return sca_lam->var(n++); });
            return unflatten(tuple, rewrite(old->dom(i)), false);
        }));
        map(old->var(), new_vars);

        sca_lam->set(rewrite(old->filter()), rewrite(old->body()));
        return sca_lam;
    }

    return RWPhase::rewrite_mut_Lam(old);
}

const Def* Scalarize::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    // If the callee's signature changed, it has been scalarized by rewrite_mut_Lam();
    // its args then need the same per-projection flattening the new signature was built with.
    // Deciding by the actual rewritten lam (instead of should_expand) keeps caller and callee in sync.
    auto& w               = new_world();
    const Def* new_callee = nullptr;

    if (auto lam = app->callee()->isa_mut<Lam>()) {
        if (auto new_lam = rewrite(lam); new_lam->type() != rewrite(lam->type())) new_callee = new_lam;
    } else if (auto proj = app->callee()->isa<Extract>()) {
        auto tuple = proj->tuple()->isa<Tuple>();
        if (tuple && tuple->num_ops() != 0 && std::all_of(tuple->ops().begin(), tuple->ops().end(), [](const Def* op) {
                return op->isa_mut<Lam>();
            })) {
            auto new_ops = DefVec(tuple->num_ops(), [&](size_t i) { return rewrite(tuple->op(i)); });
            if (new_ops[0]->type() != rewrite(tuple->op(0)->type()))
                new_callee = w.extract(w.tuple(new_ops), rewrite(proj->index()));
        }
    }

    if (new_callee) {
        auto new_args = DefVec();
        for (size_t i = 0, e = app->num_args(); i != e; ++i)
            flatten(new_args, rewrite(app->arg(e, i)), false);
        return w.app(new_callee, new_args);
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim
