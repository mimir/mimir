#pragma once

#include "mim/phase.h"

namespace mim {

/// Normalizes branches: η-expands non-Lam branch targets so that both sides of a `(f, t)#cond`
/// branch are Lam%s, as later phases and the backends expect.
class BranchNormalize : public RWPhase {
public:
    BranchNormalize(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* normalize(const Def*);
    const Def* rewrite_mut_Lam(Lam*) final;

    DefSet analyzed_;
    LamMap<bool> candidate_;
};

} // namespace mim
