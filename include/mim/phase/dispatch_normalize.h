#pragma once

#include "mim/phase.h"
#include "mim/tuple.h"

namespace mim {

/// Normalizes Dispatch%es: η-expands non-Lam targets so that every target of a `(t_0, ..., t_n)#index`
/// jump table is a Lam, as later phases and the backends expect.
class DispatchNormalize : public RWPhase {
public:
    DispatchNormalize(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_mut_Lam(Lam*) final;
    /// η-expands the targets of @p sw to Lam%s, recursively.
    const Def* normalize(Switch sw);
    const Def* eta_expand(const Def*);
};

} // namespace mim
