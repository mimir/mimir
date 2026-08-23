#include "mim/plug/cps/phase/conv.h"

#include <mim/tuple.h>

#include "mim/plug/cps/cps.h"

namespace mim::plug::cps {

/// A term-level, direct-style function that we can and want to convert.
/// Type-level and higher-order functions stay in direct style.
static bool convertible(Lam* lam) {
    if (!lam->is_set() || lam->is_external() || lam->is_annex() || Lam::isa_cn(lam)) return false;
    auto codom = lam->codom();
    return !codom->isa<Type>() && !codom->isa<Pi>();
}

const Def* Conv::map(const Def* old_def, const Def* new_def) {
    auto& old2new           = new_def->has_free_vars_in(scoped_) ? old2news_.back() : old2news_.front();
    return old2new[old_def] = new_def;
}

const Def* Conv::rewrite_mut_Lam(Lam* old_lam) {
    if (!is_bootstrapping() && convertible(old_lam)) return convert(old_lam);

    auto new_lam = new_world().mut_lam(rewrite(old_lam->type())->as<Pi>());
    map(old_lam, new_lam);
    if (!old_lam->is_set()) return new_lam;

    auto scope      = Scope(*this, !is_bootstrapping() && Lam::isa_cn(old_lam));
    auto new_filter = rewrite(old_lam->filter());
    auto new_body   = wire(scope.base(), rewrite(old_lam->body()));
    return new_lam->set(new_filter, new_body);
}

const Def* Conv::convert(Lam* old_lam) {
    auto& w      = new_world();
    auto old_pi  = old_lam->type();
    auto new_dom = rewrite(old_pi->dom());

    Lam* new_lam;
    if (auto old_var = old_pi->has_var()) {
        // dependent codom: bind it against a fresh mutable Sigma [a: A, Cn B[old_var → a]]
        auto sigma = w.mut_sigma(2);
        sigma->var();
        scoped_ = w.vars().insert(scoped_, sigma->has_var());
        push();
        map(old_var, sigma->var(2, 0));
        auto new_codom = rewrite(old_pi->codom());
        pop();
        sigma->set(0, new_dom);
        sigma->set(1, w.cn(new_codom));
        new_lam = w.mut_con(sigma);
    } else {
        new_lam = w.mut_fun(new_dom, rewrite(old_pi->codom()));
    }
    new_lam->set(old_lam->dbg_key())->debug_suffix("_cps");

    auto [param, ret] = new_lam->vars<2>();
    map(old_lam->var(), param);
    auto wrapper = map(old_lam, op_cps2ds_dep(new_lam));

    DLOG("convert {}: {} ↝ {}: {}", old_lam, old_pi, new_lam, new_lam->type());

    auto scope      = Scope(*this, true);
    auto new_filter = rewrite(old_lam->filter());
    auto tail       = w.app(ret, rewrite(old_lam->body()));
    new_lam->set(new_filter, wire(scope.base(), tail));

    return wrapper;
}

const Def* Conv::rewrite_imm_App(const App* old_app) {
    auto new_arg    = rewrite(old_app->arg());
    auto new_callee = rewrite(old_app->callee());

    if (liftable_) {
        if (auto wrapped = Axm::isa<cps2ds_dep>(new_callee)) return lift(wrapped->arg(), new_arg, old_app);

        // A dynamic dispatch hides the individual cps2ds wrappers from the
        // direct-call case above. Unwrap every target and lift the selected
        // CPS callee with one shared result continuation:
        //
        //   (cps2ds f, cps2ds g)#i arg  ->  (f, g)#i (arg, cont)
        if (auto extract = new_callee->isa<Extract>(); extract && !Lit::isa(extract->index())) {
            if (auto arity = Lit::isa(extract->tuple()->arity())) {
                auto targets = DefVec();
                targets.reserve(*arity);
                for (size_t i = 0; i != *arity; ++i) {
                    auto wrapped = Axm::isa<cps2ds_dep>(extract->tuple()->proj(*arity, i));
                    if (!wrapped) {
                        targets.clear();
                        break;
                    }
                    targets.emplace_back(wrapped->arg());
                }
                if (!targets.empty()) {
                    auto dispatch = new_world().extract(new_world().tuple(targets), extract->index());
                    return lift(dispatch, new_arg, old_app);
                }
            }
        }
    }

    return new_world().app(new_callee, new_arg);
}

const Def* Conv::lift(const Def* k, const Def* new_arg, const App* old_app) {
    auto& w   = new_world();
    auto cont = w.mut_con(rewrite(old_app->type()))->set_filter(false);
    auto res  = cont->var();
    cont->set(w.append_suffix(k->sym(), "_cont"));
    scoped_ = w.vars().insert(scoped_, cont->has_var());
    pending_.emplace_back(k, new_arg, cont);

    DLOG("lift {} ↝ {} ({}, {})", old_app, k, new_arg, cont);

    return res;
}

const Def* Conv::wire(size_t base, const Def* body) {
    auto& w = new_world();
    while (pending_.size() > base) {
        auto [callee, arg, cont] = pending_.back();
        pending_.pop_back();
        cont->set_body(body);

        // Selecting returning CPS functions directly would leave nested
        // returning Lams after branch-closure elimination, while the backend
        // only permits nested basic blocks. Dispatch through empty-domain
        // basic blocks and let each arm invoke its CPS target instead.
        if (auto extract = callee->isa<Extract>(); extract && !Lit::isa(extract->index())) {
            if (auto arity = Lit::isa(extract->tuple()->arity())) {
                auto arms = DefVec(*arity, [&](size_t i) -> const Def* {
                    auto arm = w.mut_con(w.sigma())->set("cps_dispatch");
                    arm->app(false, extract->tuple()->proj(*arity, i), Defs{arg, cont});
                    return arm;
                });
                body = w.app(w.extract(w.tuple(arms), extract->index()), w.tuple());
                continue;
            }
        }

        body = w.app(callee, w.tuple({arg, cont}));
    }
    return body;
}

} // namespace mim::plug::cps
