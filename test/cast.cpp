#include <doctest/doctest.h>

#include <mim/driver.h>
#include <mim/lattice.h>
#include <mim/tuple.h>

using namespace mim;

// Prod/Seq/Bound/Ext group several Node kinds each, so they cannot use fe::Nodeable's single `T::Node`.
// Without fe::NodeSetable, `isa<T>()` would fall back to a `dynamic_cast` - on Def::proj and
// Checker::alpha_impl_, i.e. the hottest paths in the compiler.
static_assert(!fe::Nodeable<Prod> && fe::NodeSetable<Prod>);
static_assert(!fe::Nodeable<Seq> && fe::NodeSetable<Seq>);
static_assert(!fe::Nodeable<Bound> && fe::NodeSetable<Bound>);
static_assert(!fe::Nodeable<Ext> && fe::NodeSetable<Ext>);

// The concrete leaves keep the exact-node path: fe::Nodeable is checked first.
static_assert(fe::Nodeable<Sigma> && fe::Nodeable<Tuple> && fe::Nodeable<Arr> && fe::Nodeable<Pack>);
static_assert(fe::Nodeable<Join> && fe::Nodeable<Meet> && fe::Nodeable<Top> && fe::Nodeable<Bot>);

TEST_CASE("Cast: node sets") {
    Driver driver;
    World& w = driver.world();

    SUBCASE("each union groups exactly its own nodes") {
        for (auto node : {Node::Sigma, Node::Tuple, Node::Arr, Node::Pack, Node::Join, Node::Meet, Node::Top, Node::Bot,
                          Node::Lit, Node::Var, Node::App, Node::Lam, Node::Pi, Node::Nat, Node::Idx, Node::Extract}) {
            CHECK(Prod::isa_node(node) == (node == Node::Sigma || node == Node::Tuple));
            CHECK(Seq::isa_node(node) == (node == Node::Arr || node == Node::Pack));
            CHECK(Bound::isa_node(node) == (node == Node::Join || node == Node::Meet));
            CHECK(Ext::isa_node(node) == (node == Node::Top || node == Node::Bot));
        }
    }

    SUBCASE("the casts agree on real Defs") {
        auto i32   = w.type_idx(4294967296_n);
        auto sigma = w.sigma({w.type_nat(), i32});
        auto tuple = w.tuple({w.lit_nat(0), w.lit_idx(4294967296_n, 1)});
        auto arr   = w.arr(3, w.type_nat());
        auto pack  = w.pack(3, w.lit_nat(7));
        auto bot   = w.bot(w.type_nat());
        auto top   = w.top(w.type_nat());

        CHECK(sigma->isa<Prod>());
        CHECK(tuple->isa<Prod>());
        CHECK_FALSE(arr->isa<Prod>());
        CHECK_FALSE(pack->isa<Prod>());
        CHECK_FALSE(bot->isa<Prod>());
        CHECK(arr->isa<Seq>());
        CHECK(pack->isa<Seq>());
        CHECK_FALSE(sigma->isa<Seq>());
        CHECK_FALSE(tuple->isa<Seq>());
        CHECK_FALSE(top->isa<Seq>());
        CHECK(bot->isa<Ext>());
        CHECK(top->isa<Ext>());
        CHECK_FALSE(sigma->isa<Ext>());
        CHECK_FALSE(arr->isa<Ext>());
        CHECK_FALSE(bot->isa<Bound>());
        CHECK_FALSE(sigma->isa<Bound>());

        // A cast that succeeds must yield the same pointer the static_cast would.
        CHECK(sigma->isa<Prod>() == static_cast<const Prod*>(sigma));
        CHECK(arr->isa<Seq>() == static_cast<const Seq*>(arr));
    }
}
