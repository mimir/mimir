#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class EtaRed : public RWPhase {
public:
    EtaRed(World& world)
        : RWPhase(world, "EtaRed") {}
    EtaRed(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void rewrite_external(Def*) final;
    const Def* rewrite_no_eta(const Def* def) { return RWPhase::rewrite(def); }
    const Def* rewrite(const Def*);
    const Def* rewrite_imm_Var(const Var*) final;
};

} // namespace mim
