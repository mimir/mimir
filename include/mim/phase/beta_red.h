#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class BetaRed : public InplaceRWPhase {
public:
    using Super = InplaceRWPhase;

    BetaRed(World& world)
        : InplaceRWPhase(world, "BetaRed") {}
    BetaRed(World& world, flags_t annex)
        : InplaceRWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, bool candidate); // lattice: true -> false

    const Def* rewrite(const Def* def) { return def->is_ground() ? def : Super::rewrite(def); }
    const Def* rewrite_imm_App(const App*) final;
    bool is_candidate(Lam* lam) const { return fe::assert_lookup(candidates_, lam); }

    DefSet analyzed_;
    LamMap<bool> candidates_;
};

} // namespace mim
