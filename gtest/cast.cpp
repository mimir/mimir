#include <gtest/gtest.h>

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

TEST(Cast, node_sets) {
    Driver driver;
    World& w = driver.world();

    // Exactly the nodes that each union groups - and nothing else.
    for (auto node : {Node::Sigma, Node::Tuple, Node::Arr, Node::Pack, Node::Join, Node::Meet, Node::Top, Node::Bot,
                      Node::Lit, Node::Var, Node::App, Node::Lam, Node::Pi, Node::Nat, Node::Idx, Node::Extract}) {
        EXPECT_EQ(Prod::isa_node(node), node == Node::Sigma || node == Node::Tuple);
        EXPECT_EQ(Seq::isa_node(node), node == Node::Arr || node == Node::Pack);
        EXPECT_EQ(Bound::isa_node(node), node == Node::Join || node == Node::Meet);
        EXPECT_EQ(Ext::isa_node(node), node == Node::Top || node == Node::Bot);
    }

    // And the casts agree on real Def%s.
    auto i32   = w.type_idx(4294967296_n);
    auto sigma = w.sigma({w.type_nat(), i32});
    auto tuple = w.tuple({w.lit_nat(0), w.lit_idx(4294967296_n, 1)});
    auto arr   = w.arr(3, w.type_nat());
    auto pack  = w.pack(3, w.lit_nat(7));
    auto bot   = w.bot(w.type_nat());
    auto top   = w.top(w.type_nat());

    EXPECT_TRUE(sigma->isa<Prod>() && tuple->isa<Prod>());
    EXPECT_FALSE(arr->isa<Prod>() || pack->isa<Prod>() || bot->isa<Prod>());
    EXPECT_TRUE(arr->isa<Seq>() && pack->isa<Seq>());
    EXPECT_FALSE(sigma->isa<Seq>() || tuple->isa<Seq>() || top->isa<Seq>());
    EXPECT_TRUE(bot->isa<Ext>() && top->isa<Ext>());
    EXPECT_FALSE(sigma->isa<Ext>() || arr->isa<Ext>());
    EXPECT_FALSE(bot->isa<Bound>() || sigma->isa<Bound>());

    // A cast that succeeds must yield the same pointer the static_cast would.
    EXPECT_EQ(sigma->isa<Prod>(), static_cast<const Prod*>(sigma));
    EXPECT_EQ(arr->isa<Seq>(), static_cast<const Seq*>(arr));
}
