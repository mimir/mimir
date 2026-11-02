#pragma once

#include "mim/phase.h"

namespace mim {

/// Rewrites each Join `A ∪ B` into the Variant `| A | B` whose cases are the Join's ops in their sorted order.
/// A value that inhabits a Join by subsumption is explicitly injected into its case.
/// That is the Inj of a case, a dynamic Extract of a heterogeneous tuple, or a Match arm narrower than its Match.
/// A Match on a Join gets one arm per case; an arm covering several cases gets its value re-injected into its domain.
class Join2Variant : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Injects the new_world() value @p val of old_world() type @p from into the old_world() type @p to.
    const Def* lift(const Def* to, const Def* from, const Def* val);

    bool retype(const Axm*) final;
    const Def* rewrite_imm_Join(const Join*) final;
    const Def* rewrite_imm_Inj(const Inj*) final;
    const Def* rewrite_imm_Match(const Match*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
};

} // namespace mim
