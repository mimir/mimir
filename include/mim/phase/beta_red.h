#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class BetaRed : public FPPhase {
public:
    BetaRed(World& world)
        : FPPhase(world, "BetaRed") {}
    BetaRed(World& world, flags_t annex)
        : FPPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, bool candidate); // lattice: true -> false

    /// A β-redex is an App on a *mutable* Lam and β-reduction substitutes Var%s, so a closed immutable is untouchable.
    bool skip(const Def* def) const final { return skip_closed_imm(def); }
    const Def* rewrite_imm_App(const App*) final;
    bool is_candidate(Lam* lam) const { return assert_lookup(candidates_, lam); }

    DefSet analyzed_;
    LamMap<bool> candidates_;
};

} // namespace mim
