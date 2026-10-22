#pragma once

#include "mim/phase.h"

namespace mim {

/// Eliminates all nominal newtypes, so that no Nom/Name/Struc ever reaches a backend.
/// A `nom` only distinguishes types and shares the representation of the type it wraps,
/// so `nom N = T` erases to `T` while both `v inj N` and `#v` erase to `v`.
class NomErasure : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    const Def* rewrite_imm_Nom(const Nom*) final;
    const Def* rewrite_imm_Name(const Name*) final;
    const Def* rewrite_imm_Struc(const Struc*) final;
};

} // namespace mim
