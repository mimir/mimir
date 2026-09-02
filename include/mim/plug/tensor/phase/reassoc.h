#pragma once

#include <array>
#include <optional>

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Reassociates chains of %%tensor.product_2d with the classic matrix-chain-order dynamic program,
/// so that a chain is evaluated with the least number of scalar multiplications.
///
/// Extents need not be literal: a cost is kept as a polynomial in the symbolic extents, ordered by
/// coefficient-wise `≤`.
/// Since extents are `Nat`s and hence non-negative, that order proves `≤` under *every* instantiation -
/// but it is only a *partial* order, so a chain whose optimum depends on a symbolic extent (a batch
/// dimension that favours a different bracketing when small than when large) is left as written.
class Reassoc : public RWPhase {
public:
    Reassoc(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// One `%tensor.product_2d` of a chain: `«m, k» · «k, l»`.
    struct Link {
        const App* app;
        const Def* m;
        const Def* k;
        const Def* l;
    };

    /// A bracketing as `(i, s, j)` triples: `i … j` splits after `s`.
    using Splits = fe::Vector<std::array<u64, 3>>;

    void start() override;
    const Def* rewrite_imm_App(const App*) final;

    const Def* reassoc(const App*);
    std::optional<Link> isa_link(const Def* def, const Def* ring) const;

    /// Appends the chain's leaves to @p mats, each leaf's *row* extent to @p dims, and the bracketing as
    /// written to @p orig; the caller appends the chain's trailing column extent and its own split.
    void flatten(const Def* def, const Def* ring, const Def* rows, DefVec& mats, DefVec& dims, Splits& orig);

    /// Rebuilds `mats[i … j]` in the new world, parenthesized according to @p split (indexed `i * n + j`).
    const Def* build(const Def* head, Defs mats, Defs dims, fe::View<u64> split, u64 i, u64 j);

    /// Old-world consumer count per `product_2d` app, attributed through tuple wrappers.
    DefMap<u64> consumers_;
};

} // namespace mim::plug::tensor::phase
