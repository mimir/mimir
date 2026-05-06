#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class BetaRedPhase : public RWPhase {
public:
    BetaRedPhase(World& world)
        : RWPhase(world, "BetaRedPhase") {}
    BetaRedPhase(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, bool candidate); // lattice: true -> false

    const Def* rewrite_imm_App(const App*) final;
    bool is_candidate(Lam* lam) const { return assert_lookup(candidates_, lam); }

    DefSet analyzed_;
    LamMap<bool> candidates_;
};

} // namespace mim
