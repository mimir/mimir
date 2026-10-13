#include "mim/lam.h"

#include "mim/world.h"

using namespace std::string_literals;

namespace mim {

/*
 * Pi
 */

const Pi* Pi::ret_pi() const {
    // num_doms() has to materialize a Lit for the arity of a Sigma dom - which means a full hash-cons round trip.
    // So compute it *once* and feed it to the (a, i) projection instead of letting dom(i) re-derive it.
    if (auto n = num_doms(); n != 0)
        if (auto last = dom(n, n - 1)) return Pi::isa_basicblock(last); // a frozen World yields no projection
    return nullptr;
}

Pi* Pi::set_dom(Defs doms) { return Def::set(0, world().sigma(doms))->as<Pi>(); }

/*
 * Lam
 */

Lam* Lam::forwardee() const {
    if (auto app = body()->isa<App>())
        if (auto callee = app->callee()->isa_mut<Lam>(); callee && callee->is_set()) return callee;
    return nullptr;
}

Lam* Lam::set_filter(Filter filter) { return Def::set(0, world().filter(filter))->as<Lam>(); }
Lam* Lam::set(Filter filter, const Def* body) { return Def::set({world().filter(filter), body})->as<Lam>(); }
Lam* Lam::app(Filter f, const Def* callee, const Def* arg) {
    return Def::set({world().filter(f), world().app(callee, arg)})->as<Lam>();
}
Lam* Lam::app(Filter filter, const Def* callee, Defs args) { return app(filter, callee, world().tuple(args)); }

Lam* Lam::branch(Filter filter, const Def* cond, const Def* t, const Def* f, const Def* arg) {
    return app(filter, world().select(cond, t, f), arg ? arg : world().tuple());
}

Defs Lam::reduce(Defs args) const { return Def::reduce(world().tuple(args)); }

const Def* Lam::isa_ret_arg(const Def* d) {
    auto lam = d->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return nullptr;
    auto app = lam->body()->isa<App>();
    return app && app->callee() == lam->ret_var() ? app->arg() : nullptr;
}

static const Def* eta_step(const Def* def) {
    if (auto lam = def->isa<Lam>(); lam && lam->is_set())
        if (auto app = lam->body()->isa<App>()) {
            auto var = lam->has_var();
            if (var && app->callee()->has_free_var(var)) return def;
            // The sole inhabitant of a singleton dom is η-equal to the Var.
            if (app->arg() == var || app->arg() == lam->world().inhabitant(lam->dom())) return app->callee();
        }
    return def;
}

const Def* Lam::eta_reduce(const Def* def) {
    // Floyd: the chain's end is a fixpoint of eta_step, i.e. a cycle of length 1.
    auto slow = eta_step(def), fast = eta_step(slow);
    for (; slow != fast; slow = eta_step(slow), fast = eta_step(eta_step(fast))) {}
    for (slow = def; slow != fast; slow = eta_step(slow), fast = eta_step(fast)) {}
    return fast == def ? nullptr : fast;
}

Lam* Lam::eta_expand(Filter filter, const Def* f) {
    auto& w  = f->world();
    auto eta = w.mut_lam(f->type()->as<Pi>());
    eta->set(f->dbg_key())->debug_prefix("eta_"s);
    return eta->app(filter, f, eta->var());
}

} // namespace mim
