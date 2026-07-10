#include "mim/phase/scalarize.h"

#include <algorithm>

#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

namespace {
/// A `Tuple` all of whose ops are mutable `Lam`s - i.e. a branch / dispatch table.
bool is_lam_tuple(const Def* def) {
    auto tuple = def->isa<Tuple>();
    return tuple && tuple->num_ops() != 0
        && std::all_of(tuple->ops().begin(), tuple->ops().end(), [](const Def* op) { return op->isa_mut<Lam>(); });
}

} // namespace

/*
 * Analysis - optimistic: every optimizable Lam is splittable until proven otherwise.
 */

bool Scalarize::Analysis::eligible(Lam* lam) const {
    return isa_optimizable(lam) && Pi::isa_cn(lam->type()) && lam->type()->isa_imm();
}

bool Scalarize::Analysis::splittable(Lam* lam) const { return !demoted_.contains(lam); }

void Scalarize::Analysis::demote(Lam* lam) {
    if (demoted_.emplace(lam).second) {
        DLOG("demote: {}", lam);
        invalidate();
    }
}

void Scalarize::Analysis::demote_all(const Def* lam_tuple) {
    for (auto op : lam_tuple->ops())
        if (auto lam = op->isa_mut<Lam>()) demote(lam);
}

void Scalarize::Analysis::lock(Lam* lam, size_t dom) {
    if (locked_[lam].emplace(dom).second) {
        DLOG("lock: {} #{}", lam, dom);
        invalidate();
    }
}

void Scalarize::Analysis::inspect(const Def* def) {
    // A parameter that is Extract%ed / Insert%ed via a non-constant index must not be split.
    const Def* idx_tuple = nullptr;
    const Def* idx       = nullptr;
    if (auto ex = def->isa<Extract>()) idx_tuple = ex->tuple(), idx = ex->index();
    if (auto in = def->isa<Insert>()) idx_tuple = in->tuple(), idx = in->index();
    if (idx_tuple && !Lit::isa(idx)) {
        if (auto var = idx_tuple->isa<Var>()) {
            if (auto lam = var->mut()->isa_mut<Lam>()) demote(lam); // whole var indexed dynamically
        } else if (auto proj = idx_tuple->isa<Extract>()) {
            if (auto var = proj->tuple()->isa<Var>()) {
                if (auto lam = var->mut()->isa_mut<Lam>()) {
                    if (auto i = Lit::isa(proj->index()))
                        lock(lam, *i);
                    else
                        demote(lam);
                }
            }
        }
    }

    if (def->isa<Var>()) return; // a Var references its mut as op; that is not a use of the Lam

    for (size_t i = 0, e = def->num_ops(); i != e; ++i) {
        auto op = def->op(i);
        if (!op) continue;
        auto callee_pos = def->isa<App>() && i == 0;

        if (auto lam = op->isa_mut<Lam>()) {
            // ops of a lam tuple are judged when the tuple itself is used
            if (!callee_pos && !is_lam_tuple(def)) demote(lam);
        } else if (is_lam_tuple(op)) {
            // all sides of a branch share one expansion decision: only fine if all members
            // are eligible, agree on their type, are still splittable, and are unlocked.
            auto extract_pos = def->isa<Extract>() && i == 0;
            auto consistent  = std::all_of(op->ops().begin(), op->ops().end(), [&](const Def* d) {
                auto lam = d->as_mut<Lam>();
                auto lck = locked_.find(lam);
                return d->type() == op->op(0)->type() && eligible(lam) && splittable(lam)
                    && (lck == locked_.end() || lck->second.empty());
            });
            if (!extract_pos || !consistent) demote_all(op);
        } else if (auto proj = op->isa<Extract>(); proj && is_lam_tuple(proj->tuple())) {
            if (!callee_pos) demote_all(proj->tuple());
        }
    }
}

const Def* Scalarize::Analysis::rewrite(const Def* old) {
    inspect(old);
    return mim::Analysis::rewrite(old);
}

Vector<bool> Scalarize::Analysis::plan(Lam* lam) {
    auto mask = Vector<bool>();
    if (eligible(lam) && splittable(lam)) {
        auto n   = lam->num_tvars();
        auto lck = locked_.find(lam);
        auto any = false;
        mask.assign(n, false);
        for (size_t i = 0; i != n; ++i) {
            auto locked = lck != locked_.end() && lck->second.contains(i);
            if (!locked && lam->tvar(i)->type()->num_tprojs() > 1) mask[i] = any = true;
        }
        if (!any) mask.clear();
    }
    return mask;
}

/*
 * Scalarize
 */

const Def* Scalarize::rewrite_mut_Lam(Lam* old) {
    auto mask = is_bootstrapping() ? Vector<bool>() : analysis_.plan(old);
    if (mask.empty()) return RWPhase::rewrite_mut_Lam(old);

    auto& w = new_world();
    auto n  = old->num_tvars();

    // build the flattened (one level) domain
    auto dtypes = DefVec(n);
    auto counts = Vector<size_t>(n);
    auto doms   = DefVec();
    for (size_t i = 0; i != n; ++i) {
        dtypes[i] = rewrite(old->tvar(i)->type());
        if (mask[i]) {
            auto pieces = dtypes[i]->tprojs();
            counts[i]   = pieces.size();
            doms.insert(doms.end(), pieces.begin(), pieces.end());
        } else {
            counts[i] = 1;
            doms.emplace_back(dtypes[i]);
        }
    }

    auto sca = w.mut_lam(w.cn(doms))->set(old->dbg());
    DLOG("scalarize {} : {} ~> {} : {}", old, old->type(), sca, sca->type());
    map(old, sca);

    // reassemble the old var one level from the fresh scalar vars
    auto params = DefVec();
    params.reserve(n);
    for (size_t i = 0, v = 0; i != n; ++i) {
        if (counts[i] == 1) {
            params.emplace_back(sca->var(v++));
        } else {
            auto pieces = DefVec();
            pieces.reserve(counts[i]);
            for (size_t j = 0; j != counts[i]; ++j)
                pieces.emplace_back(sca->var(v++));
            params.emplace_back(w.tuple(dtypes[i], pieces));
        }
    }
    map(old->var(), w.tuple(params));

    sca->set(rewrite(old->filter()), rewrite(old->body()));
    invalidate();
    return sca;
}

void Scalarize::flatten_args(DefVec& ops, const App* app, const Vector<bool>& mask) {
    auto n = app->num_targs();
    for (size_t i = 0; i != n; ++i) {
        auto arg = rewrite(app->targ(i));
        if (i < mask.size() && mask[i]) {
            auto pieces = arg->tprojs();
            ops.insert(ops.end(), pieces.begin(), pieces.end());
        } else {
            ops.emplace_back(arg);
        }
    }
}

const Def* Scalarize::rewrite_imm_App(const App* app) {
    if (!is_bootstrapping()) {
        auto& w               = new_world();
        const Def* new_callee = nullptr;
        auto mask             = Vector<bool>();

        if (auto lam = app->callee()->isa_mut<Lam>()) {
            if (mask = analysis_.plan(lam); !mask.empty()) new_callee = rewrite(lam);
        } else if (auto proj = app->callee()->isa<Extract>(); proj && is_lam_tuple(proj->tuple())) {
            auto tuple = proj->tuple();
            if (mask = analysis_.plan(tuple->op(0)->as_mut<Lam>()); !mask.empty()) {
                auto members = DefVec(tuple->num_ops(), [&](size_t i) { return rewrite(tuple->op(i)); });
                new_callee   = w.extract(w.tuple(members), rewrite(proj->index()));
            }
        }

        if (new_callee) {
            auto args = DefVec();
            flatten_args(args, app, mask);
            return w.app(new_callee, args);
        }
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim
