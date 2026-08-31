#include "mim/plug/clos/phase/clos_conv_prep.h"

#include <mim/nest.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos::phase {

namespace {

bool isa_cnt(const App* body, const Def* def, size_t i) {
    return Pi::isa_returning(body->callee_type()) && body->arg() == def && i == def->num_ops() - 1;
}

const Def* isa_br(const App* body, const Def* def) {
    if (!Pi::isa_cn(body->callee_type())) return nullptr;
    auto proj = body->callee()->isa<Extract>();
    return (proj && proj->tuple() == def && proj->tuple()->isa<Tuple>()) ? proj->tuple() : nullptr;
}

bool isa_callee_br(const App* body, const Def* def, size_t i) {
    if (!Pi::isa_cn(body->callee_type())) return false;
    return isa_callee(def, i) || isa_br(body, def);
}

Lam* isa_retvar(const Def* def) {
    if (auto [var, lam] = isa_var_proj<Lam>(def); var && lam && var == lam->ret_var()) return lam;
    return nullptr;
}

} // namespace

bool ClosConvPrep::analyze() {
    if (analyzed_) return false;
    analyzed_ = true;

    // Collect all returning Lams and assign each basicblock Lam in their Nest to them.
    DefSet done;
    auto visit = [&](auto&& visit, const Def* def) -> void {
        if (!done.emplace(def).second) return;

        if (auto lam = def->isa_mut<Lam>(); lam && Pi::isa_returning(lam)) {
            lam2fscope_[lam] = lam;
            log().d("scope: {} → {}", lam, lam);
            auto nest = Nest(lam);
            for (auto mut : nest.muts())
                if (auto bb_lam = Lam::isa_mut_basicblock(mut)) {
                    log().d("scope: {} → {}", bb_lam, lam);
                    lam2fscope_[bb_lam] = lam;
                }
        }

        if (auto mut = def->isa_mut()) {
            if (mut->is_set())
                for (auto op : mut->deps())
                    visit(visit, op);
        } else {
            for (auto op : def->deps())
                visit(visit, op);
        }
    };
    for (auto def : old_world().roots())
        visit(visit, def);

    return false;
}

const Def* ClosConvPrep::eta_wrap(const Def* old_op, attr a) {
    auto [entry, inserted] = old2wrapper_.emplace(old_op, nullptr);
    auto& wrapper          = entry->second;
    if (inserted) {
        auto& w = new_world();
        wrapper = w.mut_lam(rewrite(old_op->type())->as<Pi>());
        wrapper->app(false, rewrite(old_op), wrapper->var());
    }
    return new_world().call(a, wrapper);
}

const Def* ClosConvPrep::rewrite_arg(const App* app, const Def* old_op) {
    auto arg = app->arg();
    auto i   = 0u;
    for (; i < arg->num_projs(); i++)
        if (arg->proj(i) == old_op) break;

    if (auto lam = isa_retvar(old_op); lam && from_outer_scope(lam)) {
        log().d("return var from an enclosing scope: {}", old_op);
        return eta_wrap(old_op, attr::free_bb)->set("free_ret");
    }
    if (auto bb_lam = Lam::isa_mut_basicblock(old_op); bb_lam && from_outer_scope(bb_lam)) {
        log().d("BB from an enclosing scope: {}", old_op);
        return new_world().call(attr::free_bb, rewrite(old_op));
    }
    if (isa_cnt(app, arg, i)) {
        if (Axm::isa<attr>(attr::returning, old_op) || isa_retvar(old_op)) {
            return rewrite(old_op);
        } else if (auto contlam = old_op->isa_mut<Lam>()) {
            return new_world().call(attr::returning, rewrite(contlam));
        } else {
            auto wrapper = eta_wrap(old_op, attr::returning)->set("eta_cont");
            log().d("eta-expand return continuation: {} → {}", old_op, wrapper);
            return wrapper;
        }
    }

    if (!isa_callee_br(app, arg, i)) {
        if (auto bb_lam = Lam::isa_mut_basicblock(old_op)) {
            log().d("first-class use of BB: {}", bb_lam);
            return new_world().call(attr::fstclass_bb, rewrite(bb_lam));
        }
        // @note This relies on branches staying in `Extract`-of-`Tuple` form; if eta-reduction were to collapse
        // a branch back into a bare continuation, it would have to be re-wrapped here.
        if (isa_retvar(old_op)) {
            log().d("first-class use of return var: {}", old_op);
            return eta_wrap(old_op, attr::fstclass_bb)->set("fstclass_ret");
        }
    }

    return rewrite(old_op);
}

const Def* ClosConvPrep::rewrite_callee_op(const Def* old_op) {
    if (!old_op->isa_mut<Lam>()) {
        auto wrapper = eta_wrap(old_op, attr::bottom)->set("eta_br");
        log().d("eta-wrap branch: {} → {}", old_op, wrapper);
        return wrapper;
    }
    return rewrite(old_op);
}

const Def* ClosConvPrep::rewrite_imm_App(const App* app) {
    if (is_bootstrapping() || Axm::isa<attr>(app)) return RWPhase::rewrite_imm_App(app);

    // Skip if the surrounding mutable is no Lam with a continuation call as body.
    auto mut  = curr_mut() ? curr_mut()->isa_mut<Lam>() : nullptr;
    auto body = mut && mut->is_set() ? mut->body()->isa<App>() : nullptr;
    if (!body || !Pi::isa_cn(body->callee_type())) return RWPhase::rewrite_imm_App(app);

    auto& w = new_world();

    // Eta-expand branches in callee position.
    const Def* new_callee = nullptr;
    if (Pi::isa_cn(app->callee_type())) {
        if (auto br = app->callee()->isa<Extract>()) {
            auto branches = br->tuple();
            if (branches->isa<Tuple>() && branches->type()->isa<Arr>()) {
                auto new_ops
                    = DefVec(branches->num_ops(), [&](size_t i) { return rewrite_callee_op(branches->op(i)); });
                new_callee = w.extract(w.tuple(new_ops), rewrite(br->index()));
            }
        }
    }
    if (!new_callee) new_callee = rewrite(app->callee());

    // Wrap the argument's projections.
    const Def* new_arg;
    auto arg = app->arg();
    if (arg->isa<Var>()) {
        new_arg = rewrite(arg);
    } else {
        auto new_args = DefVec(arg->num_projs(), [&](size_t i) { return rewrite_arg(app, arg->proj(i)); });
        new_arg       = arg->num_projs() == 1 ? new_args[0] : w.tuple(new_args);
    }

    return w.app(new_callee, new_arg);
}

} // namespace mim::plug::clos::phase
