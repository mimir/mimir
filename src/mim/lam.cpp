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
    if (auto n = num_doms(); n != 0) return Pi::isa_basicblock(dom(n, n - 1));
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

const Def* Lam::eta_reduce() const {
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

/*
 * Helpers
 */

const Def* compose_cn(const Def* f, const Def* g) {
    auto& world = f->world();
    auto F      = f->type()->as<Pi>();
    auto G      = g->type()->as<Pi>();

    assert(Pi::isa_returning(F));
    assert(Pi::isa_returning(G));

    auto A = G->dom(2, 0);
    auto B = G->ret_dom();
    auto C = F->ret_dom();
    // The type check of codom G = dom F is better handled by the application type checking

    world.log().d("compose f (B->C): {} : {}", f, F);
    world.log().d("compose g (A->B): {} : {}", g, G);
    world.log().d("  A: {}", A);
    world.log().d("  B: {}", B);
    world.log().d("  C: {}", C);

    auto name  = "comp_"s + f->sym().str() + "_" + g->sym().str();
    auto h     = world.mut_fun(A, C)->set(name);
    auto hcont = world.mut_con(B)->set(name + "_cont");

    h->app(true, g, {h->var((nat_t)0), hcont});

    auto hcont_var = hcont->var(); // Warning: not var(0) => only one var => normalization flattens tuples down here.
    hcont->app(true, f, {hcont_var, h->var(1) /* ret_var */});

    return h;
}

} // namespace mim
