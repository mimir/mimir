#include "mim/plug/clos/pass/clos_conv_prep.h"

#include <mim/nest.h>

#include "mim/plug/clos/clos.h"

namespace mim::plug::clos {

namespace {
// FIXME: these guys do not work if another pass rewrites curr_mut()'s body
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
    if (auto [var, lam] = ca_isa_var<Lam>(def); var && lam && var == lam->ret_var()) return lam;
    return nullptr;
}

} // namespace

void ClosConvPrep::init(PassMan* man) { Pass::init(man); }

Lam* ClosConvPrep::scope(Lam* lam) { return lam2fscope_[lam]; }

void ClosConvPrep::enter() {
    if (Pi::isa_returning(curr_mut())) {
        lam2fscope_[curr_mut()] = curr_mut();
        DLOG("scope {} -> {}", curr_mut(), curr_mut());
        auto nest = Nest(curr_mut());
        for (auto mut : nest.muts()) {
            if (auto bb_lam = Lam::isa_mut_basicblock(mut)) {
                DLOG("scope {} -> {}", bb_lam, curr_mut());
                lam2fscope_[bb_lam] = curr_mut();
            }
        }
    }

    auto body = curr_mut()->body()->isa<App>();
    // Skip if the mutable is already wrapped or the body is undefined/no continuation.
    ignore_ = !(body && Pi::isa_cn(body->callee_type())) || wrapper_.contains(curr_mut());
}

const App* ClosConvPrep::rewrite_arg(const App* app) {
    auto& w  = world();
    auto arg = app->arg();

    if (arg->isa<Var>()) return app;

    for (auto i = 0u; i < arg->num_projs(); i++) {
        auto op = arg->proj(i);

        auto refine = [&](const Def* new_op) {
            const Def* args;
            if (arg == op)
                args = new_op;
            else
                args = arg->refine(i, new_op);
            return app->refine(1, args)->as<App>();
        };

        if (auto lam = isa_retvar(op); lam && from_outer_scope(lam)) {
            DLOG("found return var from enclosing scope: {}", op);
            return refine(eta_wrap(op, attr::freeBB)->set("free_ret"));
        }
        if (auto bb_lam = Lam::isa_mut_basicblock(op); bb_lam && from_outer_scope(bb_lam)) {
            DLOG("found BB from enclosing scope {}", op);
            return refine(w.call(attr::freeBB, op));
        }
        if (isa_cnt(app, arg, i)) {
            if (Axm::isa<attr>(attr::returning, op) || isa_retvar(op)) {
                return app;
            } else if (auto contlam = op->isa_mut<Lam>()) {
                return refine(w.call(attr::returning, contlam));
            } else {
                auto wrapper = eta_wrap(op, attr::returning)->set("eta_cont");
                DLOG("eta expanded return cont: {} -> {}", op, wrapper);
                return refine(wrapper);
            }
        }

        if (!isa_callee_br(app, arg, i)) {
            if (auto bb_lam = Lam::isa_mut_basicblock(op)) {
                DLOG("found firstclass use of BB: {}", bb_lam);
                return refine(w.call(attr::fstclassBB, bb_lam));
            }
            // TODO: If EtaRed eta-reduces branches, we have to wrap them again!
            if (isa_retvar(op)) {
                DLOG("found firstclass use of return var: {}", op);
                return refine(eta_wrap(op, attr::fstclassBB)->set("fstclass_ret"));
            }
        }
    }

    return app;
}

const App* ClosConvPrep::rewrite_callee(const App* app) {
    if (Pi::isa_cn(app->callee_type())) {
        if (auto br = app->callee()->isa<Extract>()) {
            auto branches = br->tuple();
            // Eta-Expand branches
            if (branches->isa<Tuple>() && branches->type()->isa<Arr>()) {
                for (size_t i = 0, e = branches->num_ops(); i != e; ++i) {
                    if (!branches->op(i)->isa_mut<Lam>()) {
                        auto wrapper = eta_wrap(branches->op(i), attr::bottom)->set("eta_br");
                        DLOG("eta wrap branch: {} -> {}", branches->op(i), wrapper);
                        branches = branches->refine(i, wrapper);
                    }
                }
                return app->refine(0, app->callee()->refine(0, branches))->as<App>();
            }
        }
    }

    return app;
}

const Def* ClosConvPrep::rewrite(const Def* def) {
    if (ignore_ || Axm::isa<attr>(def)) return def;

    if (auto app = def->isa<App>()) {
        app = rewrite_arg(app);
        app = rewrite_callee(app);
        return app;
    }

    return def;
}

} // namespace mim::plug::clos
