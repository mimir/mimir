#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <sstream>
#include <utility>

#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/phase.h>
#include <mim/rewrite.h>

#include <mim/ast/parser.h>

#include <mim/plug/core/core.h>

using namespace mim;
using namespace mim::plug;

TEST_CASE("zip") {
    Driver driver;
    World& w    = driver.world();
    auto ast    = ast::AST(w);
    auto parser = ast::Parser(ast);

    std::istringstream iss("plugin tuple;"
                           "let a = ((0I32, 1I32,  2I32), ( 3I32,  4I32,  5I32));"
                           "let b = ((6I32, 7I32,  8I32), ( 9I32, 10I32, 11I32));"
                           "let c = ((6I32, 8I32, 10I32), (12I32, 14I32, 16I32));"
                           "let r = %tuple.zip (2, (2, 3)) (2, (I32, I32), 1, I32, %core.wrap.add 0) (a, b);");
    parser.import(iss, "zip.mim");
    // auto c = parser.scopes().find({Loc(), driver.sym("c")});
    // auto r = parser.scopes().find({Loc(), driver.sym("r")});

    // CHECK(r->is_term());
    // CHECK(!r->type()->is_term());
    // CHECK(c == r);
}

TEST_CASE("World: one-tuples fold away") {
    Driver driver;
    World& w = driver.world();

    CHECK(w.tuple({w.lit_ff()}) == w.lit_ff());

    auto type = w.mut_sigma(w.type(), 2);
    type->set(Defs{w.type_nat(), w.type_nat()});
    CHECK(w.sigma({type}) == type);

    auto v = w.tuple(type, {w.lit_idx(42), w.lit_idx(1337)});
    CHECK(w.tuple({v}) == v);
}

TEST_CASE("World: dependent extract") {
    Driver driver;
    World& w = driver.world();

    auto sig = w.mut_sigma(w.type<1>(), 2); // sig = [T: *, T]
    sig->set(0, w.type<0>());
    sig->set(1, sig->var(0_u64));
    auto a = w.axm(sig);
    CHECK(a->proj(2, 1)->type() == a->proj(2, 0_u64)); // type_of(a#1_2) == a#0_1
}

TEST_CASE("Annex") {
    SUBCASE("mangle") {
        CHECK(Annex::demangle(*Annex::mangle("test")) == "test");
        CHECK(Annex::demangle(*Annex::mangle("azAZ09_")) == "azAZ09_");
        CHECK(Annex::demangle(*Annex::mangle("01234567")) == "01234567");
        CHECK_FALSE(Annex::mangle("012345678"));
        CHECK_FALSE(Annex::mangle("!"));
        // Check whether lower 16 bits are properly ignored
        CHECK(Annex::demangle(*Annex::mangle("test") | 0xFF_u64) == "test");
        CHECK(Annex::demangle(*Annex::mangle("01234567") | 0xFF_u64) == "01234567");
    }

    SUBCASE("split") {
        Driver d;
        auto [plugin, group, tag] = Annex::split(d, d.sym("%foo.bar.baz"));
        CHECK(plugin == d.sym("foo"));
        CHECK(group == d.sym("bar"));
        CHECK(tag == d.sym("baz"));
    }
}

TEST_CASE("core.trait.size") {
    Driver driver;
    driver.log().set(fe::Log::Level::Debug).set(&std::cerr);
    World& w = driver.world();
    ast::load_plugin(w, "core");

    auto size = [&w](u64 mod) { return Lit::as(op(core::trait::size, w.type_idx(mod))); };

    CHECK(size(0x0000'0000'0000'00FF_n) == 1);
    CHECK(size(0x0000'0000'0000'0100_n) == 1);
    CHECK(size(0x0000'0000'0000'0101_n) == 2);

    CHECK(size(0x0000'0000'0000'FFFF_n) == 2);
    CHECK(size(0x0000'0000'0001'0000_n) == 2);
    CHECK(size(0x0000'0000'0001'0001_n) == 4);

    CHECK(size(0x0000'0000'FFFF'FFFF_n) == 4);
    CHECK(size(0x0000'0001'0000'0000_n) == 4);
    CHECK(size(0x0000'0001'0000'0001_n) == 8);

    CHECK(size(0xFFFF'FFFF'FFFF'FFFF_n) == 8);
    CHECK(size(0x0000'0000'0000'0000_n) == 8);
}

static const Def* normalize_test_curry(const Def* type, const Def* callee, const Def* arg) {
    auto& w = arg->world();
    return w.raw_app(type, callee, w.lit_nat(42));
}

TEST_CASE("Axm: curry and trip") {
    Driver driver;
    World& w = driver.world();

    auto n   = DefVec(11, [&w](size_t i) { return w.lit_nat(i); });
    auto nat = w.type_nat();

    SUBCASE("a recursive Pi that re-enters after 3 of 5 arguments") {
        // N -> N -> N -> N -> N
        //           ^         |
        //           |         |
        //           +---------+
        auto rec = w.mut_pi(w.type())->set_dom(nat);
        rec->set_codom(w.pi(nat, w.pi(nat, rec)));
        auto pi = w.pi(nat, w.pi(nat, rec));

        auto [curry, trip] = Axm::infer_curry_and_trip(pi);
        CHECK(curry == 5);
        CHECK(trip == 3);

        auto ax = w.axm(normalize_test_curry, curry, trip, pi)->set("test_5_3");
        auto a1 = w.app(w.app(w.app(w.app(w.app(ax, n[0]), n[1]), n[2]), n[3]), n[4]);
        auto a2 = w.app(w.app(w.app(a1, n[5]), n[6]), n[7]);
        auto a3 = w.app(w.app(w.app(a2, n[8]), n[9]), n[10]);

        CHECK(a1->as<App>()->curry() == 0);
        CHECK(a2->as<App>()->curry() == 0);
        CHECK(a3->as<App>()->curry() == 0);

        std::ostringstream os;
        a3->stream(os, 0);
        CHECK(os.str() == "%test_5_3 0 1 2 3 42 5 6 42 8 9 42\n");
    }

    SUBCASE("a Pi that is its own codomain") {
        auto rec = w.mut_pi(w.type())->set_dom(nat);
        rec->set_codom(rec);

        auto [curry, trip] = Axm::infer_curry_and_trip(rec);
        CHECK(curry == 1);
        CHECK(trip == 1);

        auto ax = w.axm(normalize_test_curry, curry, trip, rec)->set("test_1_1");
        auto a1 = w.app(ax, n[0]);
        auto a2 = w.app(a1, n[1]);
        auto a3 = w.app(a2, n[2]);

        CHECK(a1->as<App>()->curry() == 0);
        CHECK(a2->as<App>()->curry() == 0);
        CHECK(a3->as<App>()->curry() == 0);

        std::ostringstream os;
        a3->stream(os, 0);
        CHECK(os.str() == "%test_1_1 42 42 42\n");
    }

    SUBCASE("a non-recursive Pi ends its trip") {
        auto pi            = w.pi(nat, w.pi(nat, w.pi(nat, w.pi(nat, nat))));
        auto [curry, trip] = Axm::infer_curry_and_trip(pi);
        CHECK(curry == 4);
        CHECK(trip == 0);

        auto ax = w.axm(normalize_test_curry, 3, 0, pi)->set("test_3_0");
        auto a1 = w.app(w.app(w.app(ax, n[0]), n[1]), n[2]);
        auto a2 = w.app(a1, n[3]);

        CHECK(a1->as<App>()->curry() == 0);
        CHECK(a2->as<App>()->curry() == Axm::Trip_End);

        std::ostringstream os;
        a2->stream(os, 0);
        CHECK(os.str() == "%test_3_0 0 1 42 3\n");
    }
}

TEST_CASE("Type: level") {
    Driver driver;
    World& w = driver.world();
    auto pi  = w.pi(w.type<7>(), w.type<2>());
    CHECK(Lit::as(pi->type()->isa<Type>()->level()) == 8);
}

TEST_CASE("Checker: alpha") {
    Driver driver;
    World& w = driver.world();
    auto pi  = w.pi(w.type_nat(), w.type_nat());

    SUBCASE("lams") {
        // λx.x
        auto lxx = w.mut_lam(pi);
        lxx->set(false, lxx->var());
        // λy.y
        auto lyy = w.mut_lam(pi);
        lyy->set(false, lyy->var());
        // λz.x
        auto lzx = w.mut_lam(pi);
        lzx->set(false, lxx->var());
        // λw.y
        auto lwy = w.mut_lam(pi);
        lwy->set(false, lyy->var());
        // λ_.x
        auto l_x = w.lam(pi, false, lxx->var());
        // λ_.y
        auto l_y = w.lam(pi, false, lyy->var());
        // λ_.0
        auto l_0 = w.lam(pi, false, w.lit_nat_0());
        // λ_.1
        auto l_1 = w.lam(pi, false, w.lit_nat_1());

        auto check = [](const Def* l1, const Def* l2, bool infer_res, bool non_infer_res) {
            CHECK(Checker::alpha<Checker::Check>(l1, l2) == infer_res);
            CHECK(Checker::alpha<Checker::Check>(l2, l1) == infer_res);
            CHECK(Checker::alpha<Checker::Test>(l1, l2) == non_infer_res);
            CHECK(Checker::alpha<Checker::Test>(l2, l1) == non_infer_res);
        };

        check(lxx, lxx, true, true);
        check(lxx, lyy, true, true);
        check(lxx, lzx, false, false);
        check(lxx, lwy, false, false);
        check(lxx, l_x, false, false);
        check(lxx, l_y, false, false);
        check(lxx, l_0, false, false);
        check(lxx, l_1, false, false);

        check(lyy, lyy, true, true);
        check(lyy, lzx, false, false);
        check(lyy, lwy, false, false);
        check(lyy, l_x, false, false);
        check(lyy, l_y, false, false);
        check(lyy, l_0, false, false);
        check(lyy, l_1, false, false);

        check(lzx, lzx, true, true);
        check(lzx, lwy, true, false);
        check(lzx, l_x, true, true);
        check(lzx, l_y, true, false);
        check(lzx, l_0, false, false);
        check(lzx, l_1, false, false);

        check(lwy, lwy, true, true);
        check(lwy, l_x, true, false);
        check(lwy, l_y, true, true);
        check(lwy, l_0, false, false);
        check(lwy, l_1, false, false);

        check(l_x, l_x, true, true);
        check(l_x, l_y, true, false);
        check(l_x, l_0, false, false);
        check(l_x, l_1, false, false);

        check(l_y, l_y, true, true);
        check(l_y, l_0, false, false);
        check(l_y, l_1, false, false);

        check(l_0, l_0, true, true);
        check(l_0, l_1, false, false);

        check(l_1, l_1, true, true);
    }

    SUBCASE("a shared Hole under a binder") {
        auto nat = w.type_nat();
        auto dep = w.mut_pi(w.type())->set_dom(nat);
        dep->set_codom(w.type_idx(dep->var())); // Π x: Nat. Idx x

        auto l1 = w.mut_lam(dep);
        auto h  = w.mut_hole(w.type_idx(l1->var())); // unset Hole whose *type* mentions l1's Var
        l1->set(false, h);
        auto l2 = w.mut_lam(dep)->set(false, h);

        CHECK(Checker::alpha<Checker::Check>(l1, l2));
        CHECK(Checker::alpha<Checker::Test>(l1, l2));
    }
}

TEST_CASE("free vars") {
    Driver driver;
    World& w = driver.world();
    auto Nat = w.type_nat();
    auto lx  = w.mut_lam(Nat, {Nat, Nat});
    auto ly  = w.mut_lam(Nat, {Nat, Nat});
    auto x   = lx->var()->set("x")->as<Var>();
    auto y   = ly->var()->set("y")->as<Var>();
    lx->set(false, w.tuple({x, y}));
    CHECK(lx->free_vars() == Vars(y));
}

TEST_CASE("free vars: fixed point") {
    Driver driver;
    World& w  = driver.world();
    auto Nat  = w.type_nat();
    auto Bool = w.type_bool();

    // con a(cond) = b ();
    // con b() = branch (cond, t, f);
    // con t() = n vt;
    // con f() = n vf;
    auto a  = w.mut_con(Bool)->set("a");
    auto b  = w.mut_con(Defs{})->set("b");
    auto f  = w.mut_con(Defs{})->set("f");
    auto t  = w.mut_con(Defs{})->set("t");
    auto n  = w.mut_con(Nat)->set("n");
    auto kt = w.mut_con(Nat)->set("kt");
    auto kf = w.mut_con(Nat)->set("kf");

    auto cond = (a->var()->set("cond"), a->has_var());
    auto vt   = (kt->var()->set("vt"), kt->has_var());
    auto vf   = (kf->var()->set("vf"), kf->has_var());

    a->app(false, b, Defs{});
    b->branch(false, cond, t, f);
    t->app(false, n, vt);
    f->app(false, n, vf);

    auto vt_vf      = w.vars().create({vt, vf});
    auto cond_vt    = w.vars().create({vt, cond});
    auto cond_vt_vf = w.vars().insert(vt_vf, cond);

    CHECK(a->free_vars() == vt_vf);
    CHECK(b->free_vars() == cond_vt_vf);
    CHECK(t->free_vars() == Vars(vt));
    CHECK(f->free_vars() == Vars(vf));

    auto mark = a->mark();
    CHECK(b->mark() == mark);
    CHECK(f->mark() == mark);
    CHECK(t->mark() == mark);
    CHECK(n->mark() == mark);

    // invalidate f by killing its FVs: con f(y) = n 23;
    f->unset()->app(false, n, w.lit_nat(23));

    CHECK(a->mark() == 0);
    CHECK(b->mark() == 0);
    CHECK(f->mark() == 0);
    CHECK(t->mark() == mark);
    CHECK(n->mark() == mark);

    CHECK(a->free_vars() == Vars(vt));
    CHECK(b->free_vars() == cond_vt);
    CHECK(t->free_vars() == Vars(vt));
    CHECK(f->free_vars() == Vars());

    CHECK(a->mark() == mark + 2);
    CHECK(b->mark() == mark + 2);
    CHECK(f->mark() == mark + 2);
    CHECK(t->mark() == mark);
    CHECK(n->mark() == mark);
}

// The example from Table 1 in "SSA without Dominance for Higher-Order Programs".
TEST_CASE("free vars: SSA without dominance, Table 1") {
    Driver driver;
    driver.log().set(fe::Log::Level::Debug).set(&std::cerr);
    World& w = driver.world();
    ast::load_plugin(w, "core");

    auto nat = w.type_nat();
    auto run = w.curr_run();

    // continuations
    auto f_ = w.mut_con({nat, w.cn(nat)})->set("f");
    auto hi = w.mut_con(nat)->set("hi");
    auto hj = w.mut_con(nat)->set("hj");
    auto bi = w.mut_con(Defs{})->set("bi");
    auto bj = w.mut_con(Defs{})->set("bj");
    auto xi = w.mut_con(Defs{})->set("xi");
    auto xj = w.mut_con(Defs{})->set("xj");

    // vars
    auto n   = f_->var(2, 0)->set("n");
    auto ret = f_->var(2, 1)->set("ret");
    auto vf  = f_->var()->as<Var>()->set("vf");
    auto i1  = hi->var()->set("i1")->as<Var>();
    auto j1  = hj->var()->set("j1")->as<Var>();
    auto j2  = w.call(core::nat::add, Defs{j1, w.lit_nat_1()});
    auto i2  = w.call(core::nat::add, Defs{i1, j1});

    // var sets
    auto vf_i1    = w.vars().create(fe::Vector<const Var*>{vf, i1});
    auto vf_j1    = w.vars().create(fe::Vector<const Var*>{vf, j1});
    auto vf_i1_j1 = w.vars().create(fe::Vector<const Var*>{vf, i1, j1});

    // connect
    f_->app(false, hi, w.lit_nat_0());
    hi->branch(false, w.call(core::ncmp::l, Defs{i1, n}), bi, xi);
    hj->branch(false, w.call(core::ncmp::l, Defs{j1, n}), bj, xj);
    bi->app(false, hj, i1);
    bj->app(false, hj, j2);
    xi->app(false, ret, i1);
    xj->app(false, hi, j2);

    CHECK(f_->mark() == 0);
    CHECK(hi->mark() == 0);
    CHECK(hj->mark() == 0);
    CHECK(bi->mark() == 0);
    CHECK(bj->mark() == 0);
    CHECK(xi->mark() == 0);
    CHECK(xj->mark() == 0);

    xi->free_vars();
    CHECK(w.curr_run() == run + 2);
    CHECK(f_->mark() == 0);
    CHECK(hi->mark() == 0);
    CHECK(hj->mark() == 0);
    CHECK(bi->mark() == 0);
    CHECK(bj->mark() == 0);
    CHECK(xi->mark() == run + 2);
    CHECK(xj->mark() == 0);
    CHECK(xi->free_vars() == vf_i1);

    f_->free_vars();
    CHECK(w.curr_run() == run + 6);
    CHECK(f_->mark() == run + 6);
    CHECK(hi->mark() == run + 6);
    CHECK(hj->mark() == run + 6);
    CHECK(bi->mark() == run + 6);
    CHECK(bj->mark() == run + 6);
    CHECK(xi->mark() == run + 2);
    CHECK(xj->mark() == run + 6);
    CHECK(f_->free_vars() == Vars());
    CHECK(hi->free_vars() == Vars(vf));
    CHECK(hj->free_vars() == Vars(vf));
    CHECK(bi->free_vars() == vf_i1);
    CHECK(bj->free_vars() == vf_j1);
    CHECK(xi->free_vars() == vf_i1);
    CHECK(xj->free_vars() == vf_j1);

    xj->unset();
    CHECK(w.curr_run() == run + 6);
    CHECK(f_->mark() == 0);
    CHECK(hi->mark() == 0);
    CHECK(hj->mark() == 0);
    CHECK(bi->mark() == 0);
    CHECK(bj->mark() == 0);
    CHECK(xj->mark() == 0);
    CHECK(xi->mark() == run + 2);
    CHECK(xi->free_vars() == vf_i1);

    xj->app(false, hi, i2);
    f_->free_vars();
    CHECK(w.curr_run() == run + 10);
    CHECK(f_->mark() == run + 10);
    CHECK(hi->mark() == run + 10);
    CHECK(hj->mark() == run + 10);
    CHECK(bi->mark() == run + 10);
    CHECK(bj->mark() == run + 10);
    CHECK(xj->mark() == run + 10);
    CHECK(xi->mark() == run + 2);
    CHECK(f_->free_vars() == Vars());
    CHECK(hi->free_vars() == Vars(vf));
    CHECK(hj->free_vars() == vf_i1);
    CHECK(bi->free_vars() == vf_i1);
    CHECK(bj->free_vars() == vf_i1_j1);
    CHECK(xi->free_vars() == vf_i1);
    CHECK(xj->free_vars() == vf_i1_j1);
}

TEST_CASE("Def::nests") {
    Driver driver;
    driver.log().set(fe::Log::Level::Debug).set(&std::cerr);
    World& w = driver.world();

    auto nat = w.type_nat();
    auto pi  = w.pi(nat, nat);
    auto f   = w.mut_lam(pi);
    auto g   = w.mut_lam(pi);
    auto h   = w.mut_lam(pi);
    auto z   = w.mut_lam({nat, nat}, nat);
    f->app(false, g, w.lit_nat(23));
    g->app(false, h, f->var());
    h->app(false, z, {h->var(), g->var()});
    z->app(false, z, z->var());

    SUBCASE("nests(Def*) on muts") {
        CHECK(f->nests(g));
        CHECK(g->nests(h));
        CHECK(f->nests(h));

        CHECK_FALSE(f->nests(f));
        CHECK_FALSE(g->nests(g));
        CHECK_FALSE(h->nests(h));
        CHECK_FALSE(g->nests(f));
        CHECK_FALSE(h->nests(g));
        CHECK_FALSE(h->nests(f));
    }

    SUBCASE("nests(Def*) on arbitrary Defs") {
        CHECK(f->nests(g->var()));  // g is nested in f
        CHECK(g->nests(h->body())); // h's body hangs below h, which is nested in g
        CHECK(f->nests(h->body())); // transitive via h's/g's free vars

        CHECK_FALSE(f->nests(f->var())); // f's var sits at f's level - like f->nests(f)
        CHECK_FALSE(f->nests(w.lit_nat(23)));
        CHECK_FALSE(g->nests(f->var()));
        CHECK_FALSE(h->nests(f->var()));
    }
}

TEST_CASE("Hole") {
    Driver driver;
    World& w = driver.world();
    auto nat = w.type_nat();
    auto n23 = w.lit_nat(23);

    auto hole1 = w.mut_hole(nat)->set("a");
    auto hole2 = w.mut_hole(nat)->set("b");
    auto hole3 = w.mut_hole(nat)->set("c");

    SUBCASE("Rewriter collapses an assigned chain") {
        hole1->set(hole2->set(hole3->set(n23)));
        auto rw = Rewriter(w);
        CHECK(rw.rewrite(hole1) == n23);
        CHECK(hole1->op() == n23);
        CHECK(hole2->op() == n23);
        CHECK(hole3->op() == n23);
    }

    SUBCASE("Rewriter stops at the unset Hole") {
        hole1->set(hole2->set(hole3));
        auto rw = Rewriter(w);
        CHECK(rw.rewrite(hole1)->isa<Hole>());
        CHECK(hole1->op() == hole3);
        CHECK(hole2->op() == hole3);
    }

    SUBCASE("zonk collapses an assigned chain") {
        hole1->set(hole2->set(hole3->set(n23)));
        CHECK(hole1->zonk() == n23);
        CHECK(hole1->op() == n23);
        CHECK(hole2->op() == n23);
        CHECK(hole3->op() == n23);
    }

    SUBCASE("zonk stops at the unset Hole") {
        hole1->set(hole2->set(hole3));
        CHECK(hole1->zonk() == hole3);
        CHECK(hole1->op() == hole3);
        CHECK(hole2->op() == hole3);
    }
}

namespace {

// An Analysis that never converges: every round claims to have learned something new.
class Oscillate : public Analysis {
public:
    Oscillate(World& world)
        : Analysis(world, "oscillate") {}

private:
    void finalize() override { invalidate(); }
};

class OscPhase : public RWPhase {
public:
    OscPhase(World& world)
        : RWPhase(world, "osc_phase", &analysis_)
        , analysis_(world) {}

private:
    Oscillate analysis_;
};

/// Minimal propagation analysis: joins the (abstract) argument into the callee's var at every App site.
class Prop : public Analysis {
public:
    Prop(World& world, bool sparse)
        : Analysis(world, "prop") {
        if (!sparse) make_dense();
    }

    void run_fixed_point() {
        for (bool todo = true; todo;) {
            reset();
            run();
            todo = this->todo();
        }
    }

private:
    const Def* join(const Def* var, const Def* def) {
        auto cur = lattice(var);
        if (!cur) return lattice(var, def), def;
        if (cur == def || cur == var) return cur;
        return pin(var), var;
    }

    const Def* rewrite_imm_App(const App* app) override {
        if (auto lam = app->callee()->isa_mut<Lam>(); lam && lam->is_set()) {
            auto j = join(lam->var(), rewrite(app->arg()));
            // Seed the abstract value into the rewriter map so the callee's body consumes it:
            // a no-op on the lattice when nothing changed, but re-installs the map entry each round.
            lattice(lam->var(), j);
        }
        return Analysis::rewrite_imm_App(app);
    }
};

} // namespace

TEST_CASE("Phase: fixed-point iteration cap") {
    Driver driver;
    driver.flags().max_fp_iters = 8;
    CHECK_THROWS_AS(Phase::run<OscPhase>(driver.world()), std::logic_error);
}

TEST_CASE("Analysis: sparse and dense reach the same fixed point") {
    Driver driver;
    World& w = driver.world();
    auto nat = w.type_nat();
    auto pi  = w.pi(nat, nat);

    auto f = w.mut_lam(pi)->set(w.sym("f")); // f x = x
    f->set(w.lit_ff(), f->var());

    auto g = w.mut_lam(pi)->set(w.sym("g")); // g x = f x - facts about g's var flow onward into f's var via g's body
    g->set(w.lit_ff(), w.app(f, g->var()));

    auto r = w.mut_lam(pi)->set(w.sym("r")); // r x = r 1 - self-recursive so the fixed point needs several rounds
    r->set(w.lit_ff(), w.app(r, w.lit_nat(1)));

    auto main1 = w.mut_lam(pi)->set(w.sym("main1")); // main1 x = g 5
    main1->set(w.lit_ff(), w.app(g, w.lit_nat(5)));
    main1->externalize();

    auto main2 = w.mut_lam(pi)->set(w.sym("main2")); // main2 x = g (r 2) - joins a second value into g's var
    main2->set(w.lit_ff(), w.app(g, w.app(r, w.lit_nat(2))));
    main2->externalize();

    Prop dense(w, false);
    dense.run_fixed_point();
    auto dense_lattice = Def2Def(dense.lattice());

    Prop sparse(w, true);
    sparse.run_fixed_point();
    const auto& sparse_lattice = sparse.lattice();

    // r's var joins 1 (self) and 2 (main) -> ⊤; sanity-check we computed something non-trivial
    CHECK(dense.is_top(r->var()));
    CHECK_FALSE(dense_lattice.empty());

    CHECK(dense_lattice.size() == sparse_lattice.size());
    for (auto [concr, abstr] : dense_lattice) {
        auto i = sparse_lattice.find(concr);
        REQUIRE_MESSAGE(i != sparse_lattice.end(), "missing lattice entry in sparse run");
        CHECK_MESSAGE(i->second == abstr, "diverging lattice entry");
    }
}
