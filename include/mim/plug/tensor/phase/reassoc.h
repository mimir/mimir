#pragma once

#include <optional>

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// One node of a bracketing: `i … j` splits after `s`.
/// Spelling the fields out keeps `size_t` out of the type - it is *not* mim::u64 everywhere.
struct Split {
    u64 i, s, j;
};

/// A bracketing of a matrix chain, innermost node first.
using Splits = fe::Vector<Split>;

/// Reassociates chains of %%tensor.product_2d with the classic matrix-chain-order dynamic program,
/// so that a chain is evaluated with the least number of scalar multiplications.
///
/// Extents need not be literal: a cost is kept as a polynomial in the symbolic extents, ordered by
/// coefficient-wise `≤`.
/// Since extents are `Nat`s and hence non-negative, that order proves `≤` under *every* instantiation -
/// but it is only a *partial* order, so a chain can have several bracketings that each win for some
/// instantiation (a batch dimension favouring a different one when small than when large).
/// Up to Reassoc::max_dispatch_ matrices those survivors are emitted side by side behind a runtime
/// comparison of their costs; a longer chain is left as written.
class Reassoc : public RWPhase {
public:
    Reassoc(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    static constexpr u64 Default_max_dispatch = 4;

    /// One `%tensor.product_2d` of a chain: `«m, k» · «k, l»`.
    struct Link {
        const App* app;
        const Def* m;
        const Def* k;
        const Def* l;
    };

    void start() override;
    const Def* rewrite_imm_App(const App*) final;

    const Def* reassoc(const App*);
    std::optional<Link> isa_link(const Def* def, const Def* ring) const;

    /// Appends the chain's leaves to @p mats, each leaf's *row* extent to @p dims, and the bracketing as
    /// written to @p orig; the caller appends the chain's trailing column extent and its own split.
    void flatten(const Def* def, const Def* ring, const Def* rows, DefVec& mats, DefVec& dims, Splits& orig);

    /// Rebuilds `mats[i … j]` in the new world, parenthesized according to @p split (indexed `i * n + j`).
    const Def* build(const Def* head, Defs mats, Defs dims, fe::View<u64> split, u64 i, u64 j);

    /// Emits every bracketing in @p cands as a thunk and selects the cheapest one by comparing their costs
    /// at run time.
    const Def* dispatch(const Def* head, const Def* res_ty, Defs mats, Defs dims, fe::View<Splits> cands);

    /// The number of multiplications @p splits costs, as a `Nat` expression in the new world.
    const Def* cost_expr(Defs dims, const Splits& splits);

    /// Old-world consumer count per `product_2d` app, attributed through tuple wrappers.
    DefMap<u64> consumers_;

    /// Longest chain whose bracketings are enumerated - and, failing a unique winner, dispatched over.
    /// The number of bracketings is `Catalan(n − 1)`, so this cannot grow much.
    /// Set with `-X tensor:reassoc-max=<n>`; below `3` nothing is ever dispatched.
    u64 max_dispatch_ = Default_max_dispatch;
};

} // namespace mim::plug::tensor::phase
