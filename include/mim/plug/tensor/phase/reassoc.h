#pragma once

#include <optional>

#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Reassociates chains of %%tensor.product_2d with the classic matrix-chain-order dynamic program,
/// so that a chain is evaluated with the least number of scalar multiplications.
/// Restricted to %%tensor.product_2d whose extents are all Lit%erals.
class Reassoc : public RWPhase {
public:
    Reassoc(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    /// One `%tensor.product_2d` of a chain: `«m, k» · «k, l»`.
    struct Link {
        const App* app;
        u64 m, k, l;
    };

    void start() override;
    const Def* rewrite_imm_App(const App*) final;

    const Def* reassoc(const App*);
    std::optional<Link> isa_link(const Def* def, const Def* ring) const;

    /// Appends the chain's leaves to @p mats, each leaf's *row* extent to @p dims, and the cost of the
    /// chain as written to @p cost; the caller appends the chain's trailing column extent.
    void flatten(const Def* def, const Def* ring, u64 rows, DefVec& mats, fe::Vector<u64>& dims, u64& cost);

    /// Rebuilds `mats[i … j]` in the new world, parenthesized according to @p split (indexed `i * n + j`).
    const Def* build(const Def* head, Defs mats, fe::View<u64> dims, fe::View<u64> split, u64 i, u64 j);

    /// Old-world consumer count per `product_2d` app, attributed through tuple wrappers.
    DefMap<u64> consumers_;
};

} // namespace mim::plug::tensor::phase
