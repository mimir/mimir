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

// TODO maybe we can eta-reduce immutable Lams in some edge casess like: lm _: [] = f ();

const Def* Lam::isa_ret_arg(const Def* d) {
    auto lam = d->isa_mut<Lam>();
    if (!lam || !lam->is_set()) return nullptr;
    auto app = lam->body()->isa<App>();
    return app && app->callee() == lam->ret_var() ? app->arg() : nullptr;
}

const Def* Lam::eta_reduce() const {
    if (!is_set()) return nullptr;
    if (auto var = has_var()) {
        if (auto app = body()->isa<App>())
            if (app->arg() == var && !app->callee()->has_free_var(var)) return app->callee();
    }
    return nullptr;
}

Lam* Lam::eta_expand(Filter filter, const Def* f) {
    auto& w  = f->world();
    auto eta = w.mut_lam(f->type()->as<Pi>());
    eta->set(f->dbg_key())->debug_prefix("eta_"s);
    return eta->app(filter, f, eta->var());
}

} // namespace mim
