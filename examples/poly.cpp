#include <mim/world.h>

using namespace mim;

Lam* build_poly_id(World& w) {
    auto pi = w.mut_pi(w.type<1>(), /*implicit*/ true)->set_dom(w.type());
    auto pT = pi->var()->set("T");
    pi->set_codom(w.pi(pT, pT)); // pi = {T: *} -> T -> T

    auto lamT = w.mut_lam(pi);         // outer lambda
    auto lT   = lamT->var()->set("T"); // note: this is lamT's T, NOT pi's T

    auto lamx = w.mut_lam(w.pi(lT, lT)); // inner lambda
    auto x    = lamx->var()->set("x");

    lamx->set(/*tt filter*/ true, x);
    lamT->set(/*tt filter*/ true, lamx); // λ {T: *} (x: T): T = x
    return lamT;
}
