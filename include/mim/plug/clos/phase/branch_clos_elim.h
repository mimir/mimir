#pragma once

#include <mim/phase.h>

namespace mim::plug::clos {

/// Flattens branches over closure literals back into a direct branch over Lam%s.
class BranchClosElim : public RWPhase {
public:
    BranchClosElim(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    DefMap<Lam*> branch2dropped_;
};

}; // namespace mim::plug::clos
