#include <doctest/doctest.h>

#include <mim/check.h>
#include <mim/driver.h>

using namespace mim;

TEST_CASE("Level: normalization") {
    Driver driver;
    World& w = driver.world();
    auto a   = w.mut_hole_level();
    auto b   = w.mut_hole_level();

    CHECK(w.lmax({a}) == a);
    CHECK(w.lmax({a, w.lit_level(0)}) == a);
    CHECK(w.lmax({w.lit_level(1), w.lit_level(3)}) == w.lit_level(3));
    CHECK(w.lmax({a, w.lmax({b, w.lit_level(1)})}) == w.lmax({w.lit_level(1), b, a}));

    CHECK(w.lsucc(w.lsucc(a, 1), 2) == w.lsucc(a, 3));
    CHECK(w.lsucc(w.lmax({a, b}), 1) == w.lmax({w.lsucc(a, 1), w.lsucc(b, 1)}));
    CHECK(w.lsucc(w.lmax({a, w.lit_level(1)}), 1) == w.lmax({w.lsucc(a, 1), w.lit_level(2)}));

    CHECK(w.level({w.type_nat(), w.type<1>()}) == w.lit_level(2));
    CHECK_THROWS(w.level({w.type_level()}));
}

TEST_CASE("Level: exact constraints are solved right away") {
    Driver driver;
    World& w = driver.world();
    auto a   = w.mut_hole_level();
    auto b   = w.mut_hole_level();

    SUBCASE("max(a, b) = 0") {
        CHECK(Check::alpha(w.lmax({a, b}), w.lit_level(0)));
        CHECK(a->zonk() == w.lit_level(0));
        CHECK(b->zonk() == w.lit_level(0));
    }

    SUBCASE("max(a, 1) = 2") {
        CHECK(Check::alpha(w.lmax({a, w.lit_level(1)}), w.lit_level(2)));
        CHECK(a->zonk() == w.lit_level(2));
    }

    SUBCASE("max(a, 2) = 1") { CHECK_FALSE(Check::alpha(w.lmax({a, w.lit_level(2)}), w.lit_level(1))); }

    SUBCASE("a + 1 = 3") {
        CHECK(Check::alpha(w.lsucc(a, 1), w.lit_level(3)));
        CHECK(a->zonk() == w.lit_level(2));
    }

    SUBCASE("a + 2 = 1") { CHECK_FALSE(Check::alpha(w.lsucc(a, 2), w.lit_level(1))); }

    SUBCASE("a + 1 = b + 3") {
        CHECK(Check::alpha(w.lsucc(a, 1), w.lsucc(b, 3)));
        CHECK(a->zonk() == w.lsucc(b, 2));
    }

    SUBCASE("max(a + 1, 1) = 3") {
        CHECK(Check::alpha(w.lmax({w.lsucc(a, 1), w.lit_level(1)}), w.lit_level(3)));
        CHECK(a->zonk() == w.lit_level(2));
    }

    SUBCASE("Test mode leaves Holes alone") {
        CHECK_FALSE(Check::test_alpha(w.lmax({a, b}), w.lit_level(0)));
        CHECK(Hole::isa_unset(a));
    }

    w.solve_levels();
}

TEST_CASE("Level: underdetermined constraints are deferred") {
    Driver driver;
    World& w = driver.world();
    auto a   = w.mut_hole_level();
    auto b   = w.mut_hole_level();

    SUBCASE("max(a, 1) = 1 defaults a to 0") {
        CHECK(Check::alpha(w.lmax({a, w.lit_level(1)}), w.lit_level(1)));
        CHECK(Hole::isa_unset(a));
        w.solve_levels();
        CHECK(a->zonk() == w.lit_level(0));
    }

    SUBCASE("max(a, b) = 1 is pinned down by a later b = 1") {
        CHECK(Check::alpha(w.lmax({a, b}), w.lit_level(1)));
        b->set(w.lit_level(1));
        w.solve_levels();
        CHECK(a->zonk() == w.lit_level(0));
    }

    SUBCASE("max(a, b) = 2 is pinned down by a later b = 0") {
        CHECK(Check::alpha(w.lmax({a, b}), w.lit_level(2)));
        b->set(w.lit_level(0));
        w.solve_levels();
        CHECK(a->zonk() == w.lit_level(2));
    }

    SUBCASE("max(a, b) = max(c, d) defaults everything to 0") {
        auto c = w.mut_hole_level();
        auto d = w.mut_hole_level();
        CHECK(Check::alpha(w.lmax({a, b}), w.lmax({c, d})));
        w.solve_levels();
        for (auto h : {a, b, c, d})
            CHECK(h->zonk() == w.lit_level(0));
    }

    SUBCASE("max(a, b) = 1 is ambiguous") {
        CHECK(Check::alpha(w.lmax({a, b}), w.lit_level(1)));
        CHECK_THROWS(w.solve_levels());
    }

    SUBCASE("a failing check defers nothing") {
        auto pi1 = w.pi(w.type(w.lmax({a, b})), w.type_nat());
        auto pi2 = w.pi(w.type<1>(), w.type_bool());
        CHECK_FALSE(Check::alpha(pi1, pi2));
        w.solve_levels();
        CHECK(Hole::isa_unset(a));
        CHECK(Hole::isa_unset(b));
    }
}
