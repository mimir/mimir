#pragma once

#include "mim/phase.h"

namespace mim {

/// Rewrites each Join `A ∪ B` into the Variant `| A | B` whose cases are the Join's ops in their sorted order.
/// A value that used to inhabit a Join by subsumption - the Inj of a case, a dynamic Extract of a heterogeneous tuple,
/// or a Match arm whose result is narrower than the Match - is explicitly injected into its case.
/// A Match on a Join gets one arm per case; an arm that covered several cases receives its value re-injected into the
/// Variant of its own domain.
class Join2Variant : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// Injects the new_world() value @p val of old_world() type @p from into the old_world() type @p to.
    const Def* lift(const Def* to, const Def* from, const Def* val);

    const Def* rewrite_imm_Join(const Join*) final;
    const Def* rewrite_imm_Inj(const Inj*) final;
    const Def* rewrite_imm_Match(const Match*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
};

} // namespace mim
