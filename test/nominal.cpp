#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/rewrite.h>

#include <mim/phase/nom_erasure.h>

using namespace mim;

TEST_CASE("Nom: distinct flags give distinct types") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nominal(0x1000, i32);
    auto foot  = w.nominal(0x1001, i32);

    REQUIRE(meter->isa<Nom>());
    CHECK(meter->as<Nom>()->op() == i32);
    CHECK(meter->type() == i32->type());
    CHECK(meter != i32);
    CHECK(meter != foot);
    CHECK(meter == w.nominal(0x1000, i32));
    CHECK(!Checker::alpha<Checker::Check>(meter, foot));
}

TEST_CASE("Nom: wrap and unwrap") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nominal(0x1000, i32);
    auto v     = w.lit_i32(23);

    auto wrapped = w.wrap(meter, v);
    REQUIRE(wrapped->isa<Wrap>());
    CHECK(wrapped->type() == meter);
    CHECK(w.unwrap(wrapped) == v);

    // An opaque value keeps the Unwrap.
    auto var  = w.mut_con(meter)->var();
    auto unwr = w.unwrap(var);
    REQUIRE(unwr->isa<Unwrap>());
    CHECK(unwr->type() == i32);
    CHECK(w.wrap(meter, unwr)->isa<Wrap>());
}

TEST_CASE("Nom: rewriting keeps the identity") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nominal(0x1000, i32);
    auto rw    = Rewriter(w);

    CHECK(rw.rewrite(meter) == meter);
    CHECK(rw.rewrite(w.wrap(meter, w.lit_i32(23))) == w.wrap(meter, w.lit_i32(23)));
}

TEST_CASE("NomErasure: nominals erase to what they wrap") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nominal(0x1000, i32);
    auto lam   = w.mut_lam(w.pi(meter, meter))->set("id");
    lam->set(false, w.wrap(meter, w.unwrap(lam->var())));
    w.externals().externalize(lam);

    Phase::run<NomErasure>(w, "nom_erasure");

    auto new_lam = w.externals()[w.sym("id")];
    REQUIRE(new_lam);
    CHECK(new_lam->type() == w.pi(w.type_i32(), w.type_i32()));
    CHECK(new_lam->as<Lam>()->body() == new_lam->as<Lam>()->var());
}
