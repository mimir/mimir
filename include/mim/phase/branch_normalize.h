#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
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
