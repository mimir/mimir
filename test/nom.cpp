#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/rewrite.h>

#include <mim/phase/nom_erasure.h>

using namespace mim;

TEST_CASE("Nom: each nom is a distinct type") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(1, i32);
    auto foot  = w.nom(2, i32);

    CHECK(meter->op() == i32);
    CHECK(meter->type() == i32->type());
    CHECK(meter == w.nom(1, i32));
    CHECK(meter != i32);
    CHECK(meter != foot);
    CHECK(Checker::alpha<Checker::Check>(meter, meter));
    CHECK(!Checker::alpha<Checker::Check>(meter, foot));
    CHECK(!Checker::alpha<Checker::Check>(meter, i32));
}

TEST_CASE("Nom: name and struc") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(1, i32);
    auto v     = w.lit_i32(23);

    auto named = w.name(meter, v);
    REQUIRE(named->isa<Name>());
    CHECK(named->type() == meter);
    CHECK(w.struc(named) == v);

    // An opaque value keeps the Struc.
    auto var   = w.mut_con(meter)->var();
    auto struc = w.struc(var);
    REQUIRE(struc->isa<Struc>());
    CHECK(struc->type() == i32);
    CHECK(w.name(meter, struc)->isa<Name>());
}

TEST_CASE("Nom: rewriting keeps the key") {
    Driver driver;
    World& w = driver.world();

    auto meter = w.nom(1, w.type_i32());
    auto rw    = Rewriter(w);

    CHECK(rw.rewrite(meter) == meter);
    CHECK(rw.rewrite(w.name(meter, w.lit_i32(23))) == w.name(meter, w.lit_i32(23)));
}

TEST_CASE("Nom: instances of a generic nom") {
    Driver driver;
    World& w = driver.world();

    // Π T: *. L T - bound twice, so the two instances only agree up to renaming.
    auto mk = [&](flags_t key) {
        auto pi = w.mut_pi(w.type<1>())->set_dom(w.type<0>());
        return pi->set_codom(w.nom(key, pi->var()));
    };

    auto pi = mk(1);
    CHECK(pi->reduce(w.type_nat()) == w.nom(1, w.type_nat()));
    CHECK(Checker::alpha<Checker::Check>(pi, mk(1)));
    CHECK(!Checker::alpha<Checker::Check>(pi, mk(2)));
    CHECK(!Checker::alpha<Checker::Check>(w.nom(1, w.type_nat()), w.nom(1, w.type_bool())));
}

TEST_CASE("NomErasure: nominals erase to what they wrap") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(1, i32);
    auto lam   = w.mut_lam(w.pi(meter, meter))->set("id");
    lam->set(false, w.name(meter, w.struc(lam->var())));
    w.externals().externalize(lam);

    Phase::run<NomErasure>(w, "nom_erasure");

    auto new_lam = w.externals()[w.sym("id")];
    REQUIRE(new_lam);
    CHECK(new_lam->type() == w.pi(w.type_i32(), w.type_i32()));
    CHECK(new_lam->as<Lam>()->body() == new_lam->as<Lam>()->var());
}
