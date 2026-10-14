#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program or are trivial forwarders.
class BetaRed : public RWPhase {
public:
    BetaRed(World& world)
        : RWPhase(world, "BetaRed") {}
    BetaRed(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    bool analyze() final;
    void analyze(const Def*);
    void visit(const Def*, bool candidate); // lattice: true -> false

    const Def* rewrite_imm_App(const App*) final;
    const Def* reduce(Lam*, const Def* old_arg);
    bool is_once(Lam* lam) const { return fe::assert_lookup(once_, lam); }
    bool is_candidate(Lam* lam) const { return !cyclic_.contains(lam) && (is_once(lam) || trivial_.contains(lam)); }

    DefSet analyzed_;
    DefSet on_stack_;
    LamMap<bool> once_;
    LamSet trivial_;
    LamSet cyclic_;
};

} // namespace mim
