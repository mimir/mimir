#include <doctest/doctest.h>

#include <mim/check.h>
#include <mim/driver.h>
#include <mim/rewrite.h>

using namespace mim;

TEST_CASE("Variant: cases are positional and free") {
    Driver driver;
    World& w = driver.world();

    auto unit = w.sigma();
    auto nat  = w.type_nat();
    auto b    = w.type_bool();

    // Where a Join collapses, a Variant keeps every case.
    CHECK(w.join({unit, unit, unit}) == unit);
    auto color = w.variant({unit, unit, unit});
    REQUIRE(color->isa<Variant>());
    CHECK(color->num_ops() == 3);

    CHECK(w.variant({nat, nat})->num_ops() == 2);
    CHECK(w.variant({nat, b}) != w.variant({b, nat}));
    CHECK(w.variant({nat, b}) == w.variant({nat, b}));
    CHECK(w.variant({nat})->isa<Variant>());
    CHECK(w.variant({})->isa<Variant>());
}

TEST_CASE("Variant: Inj carries its case") {
    Driver driver;
    World& w = driver.world();

    auto unit  = w.sigma();
    auto color = w.variant({unit, unit, unit});

    auto red   = w.inj(color, 0, w.tuple());
    auto green = w.inj(color, 1, w.tuple());
    REQUIRE(red->isa<Inj>());
    CHECK(red->as<Inj>()->index() == 0);
    CHECK(green->as<Inj>()->index() == 1);
    CHECK(red != green);
    CHECK(red == w.inj(color, 0, w.tuple()));
    CHECK(!Checker::alpha<Checker::Test>(red, green));

    CHECK_THROWS(w.inj(color, 3, w.tuple()));
    CHECK_THROWS(w.inj(color, w.tuple()));
    CHECK_THROWS(w.inj(w.variant({w.type_nat()}), 0, w.tuple()));
}

TEST_CASE("Variant: Match dispatches positionally") {
    Driver driver;
    World& w = driver.world();

    auto unit  = w.sigma();
    auto nat   = w.type_nat();
    auto color = w.variant({unit, unit, unit});
    auto arm   = [&](nat_t i) { return w.lam(w.pi(unit, nat), false, w.lit_nat(i)); };
    auto arms  = DefVec{arm(0), arm(1), arm(2)};

    auto match = [&](const Def* scrutinee) {
        auto ops = DefVec{scrutinee};
        ops.append_range(arms);
        return w.match(ops);
    };

    for (nat_t i = 0; i != 3; ++i)
        CHECK(match(w.inj(color, i, w.tuple())) == w.lit_nat(i));

    // Two arms with the same payload type are still told apart by position.
    auto f   = w.mut_lam(w.pi(color, nat));
    auto res = match(f->var());
    REQUIRE(res->isa<Match>());
    CHECK(res->as<Match>()->num_arms() == 3);

    CHECK_THROWS(w.match({w.inj(color, 0, w.tuple()), arm(0), arm(1)}));
    CHECK_THROWS(w.match({f->var(), arm(0), arm(1), w.lam(w.pi(nat, nat), false, w.lit_nat(2))}));
}

TEST_CASE("Variant: recursion") {
    Driver driver;
    World& w = driver.world();

    auto unit = w.sigma();
    auto nat  = w.type_nat();

    // rec List = | Nil | Cons: [Nat, List]
    auto list = [&]() {
        auto l = w.mut_variant(w.type(), 2);
        l->set(Defs{unit, w.sigma({nat, l})});
        return l;
    };

    auto l1 = list();
    CHECK(!l1->is_immutabilizable());
    CHECK(l1->immutabilize() == nullptr);

    // Equi-recursive: two same-shaped recursive Variants are α-equivalent.
    auto l2 = list();
    CHECK(l1 != l2);
    CHECK(Checker::alpha<Checker::Test>(l1, l2));

    auto nil  = w.inj(l1, 0, w.tuple());
    auto cons = w.inj(l1, 1, w.tuple({w.lit_nat(23), nil}));
    REQUIRE(cons->isa<Inj>());
    CHECK(cons->type() == l1);

    // A mutable Variant that does not refer to itself is just the immutable one.
    auto accidental = w.mut_variant(w.type(), 2);
    accidental->set(Defs{unit, nat});
    CHECK(accidental->immutabilize() == w.variant({unit, nat}));
}

TEST_CASE("Variant: rewriting keeps the case") {
    Driver driver;
    World& w = driver.world();

    auto nat = w.type_nat();
    auto v   = w.variant({nat, nat});
    auto inj = w.inj(v, 1, w.lit_nat(3));

    Rewriter rw(w);
    rw.map(w.lit_nat(3), w.lit_nat(4));
    CHECK(rw.rewrite(inj) == w.inj(v, 1, w.lit_nat(4)));
}
