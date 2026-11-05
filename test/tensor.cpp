#include <sstream>

#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/nom.h>
#include <mim/rewrite.h>
#include <mim/single.h>

#include <mim/ast/parser.h>

#include <mim/plug/tensor/tensor.h>

using namespace mim;
using namespace mim::plug;

TEST_CASE("Tensor: nominal indices survive normalization and rewriting") {
    Driver driver;
    auto& w = driver.world();
    w.set("nomtest");
    auto ast    = ast::AST(w);
    auto parser = ast::Parser(ast);
    auto source = std::istringstream("plugin tensor;");
    auto file   = parser.import(source, "nomtest.mim");
    REQUIRE(file);
    file->bind(ast);
    ast.error().ack();
    file->compile(ast);
    driver.move_to_library(w);

    auto ctor = w.annex<tensor::Tensor>();
    auto gid  = w.curr_gid();
    auto key  = tensor::tensor_key(w);
    CHECK(w.curr_gid() == gid);
    auto make
        = [&](nat_t r, const Def* s, const Def* T) { return w.app(w.app(w.app(ctor, w.lit_nat(r)), s), T)->as<Nom>(); };
    auto empty     = make(1, w.lit_nat(0), w.type_i32());
    auto other     = make(1, w.lit_nat(0), w.type_i64());
    auto scalar    = make(0, w.tuple(), w.type_i32());
    auto singleton = make(1, w.lit_nat(1), w.type_i32());
    auto row       = make(1, w.lit_nat(4), w.type_i32());
    auto matrix    = make(2, w.tuple({w.lit_nat(1), w.lit_nat(4)}), w.type_i32());

    CHECK(empty->key() == key);
    auto inherited = w.inherit();
    CHECK(tensor::tensor_key(*inherited) == key);
    CHECK(empty->key() == other->key());
    CHECK(empty->key() == scalar->key());
    CHECK(empty->key() == matrix->key());
    CHECK(empty == make(1, w.lit_nat(0), w.type_i32()));
    CHECK(!Check::alpha(empty, other));
    CHECK(!Check::alpha(scalar, singleton));
    CHECK(!Check::alpha(row, matrix));
    CHECK(!Check::alpha(scalar, w.type_i32()));
    CHECK(empty->type() == w.type());

    auto witness     = empty->op()->proj(2, 0)->as<Single>()->op();
    auto [r, s, tag] = witness->projs<3>();
    CHECK(r == w.lit_nat(1));
    CHECK(s == w.lit_nat(0));
    CHECK(tag->type() == w.type_i32());
    auto rw = Rewriter(w);
    CHECK(rw.rewrite(empty)->as<Nom>()->key() == empty->key());
}
