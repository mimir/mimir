#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/rewrite.h>

#include <mim/phase/nom_erasure.h>

using namespace mim;

TEST_CASE("Nom: each nom is a distinct type") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(i32);
    auto foot  = w.nom(i32);

    CHECK(meter->op() == i32);
    CHECK(meter->type() == i32->type());
    CHECK(!meter->immutabilize());
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
    auto meter = w.nom(i32);
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

TEST_CASE("Nom: rewriting stubs a new nom") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(i32);
    auto rw    = Rewriter(w);

    auto new_meter = rw.rewrite(meter);
    REQUIRE(new_meter->isa_mut<Nom>());
    CHECK(new_meter != meter);
    CHECK(new_meter->as<Nom>()->op() == i32);
    CHECK(rw.rewrite(meter) == new_meter);
    CHECK(rw.rewrite(w.name(meter, w.lit_i32(23))) == w.name(new_meter, w.lit_i32(23)));
}

TEST_CASE("NomErasure: nominals erase to what they wrap") {
    Driver driver;
    World& w = driver.world();

    auto i32   = w.type_i32();
    auto meter = w.nom(i32);
    auto lam   = w.mut_lam(w.pi(meter, meter))->set("id");
    lam->set(false, w.name(meter, w.struc(lam->var())));
    w.externals().externalize(lam);

    Phase::run<NomErasure>(w, "nom_erasure");

    auto new_lam = w.externals()[w.sym("id")];
    REQUIRE(new_lam);
    CHECK(new_lam->type() == w.pi(w.type_i32(), w.type_i32()));
    CHECK(new_lam->as<Lam>()->body() == new_lam->as<Lam>()->var());
}
