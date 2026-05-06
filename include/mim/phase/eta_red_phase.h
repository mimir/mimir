#pragma once

#include "mim/phase.h"

namespace mim {

/// Inlines in post-order all Lam%s that occur exactly *once* in the program.
class EtaRedPhase : public RWPhase {
public:
    EtaRedPhase(World& world)
        : RWPhase(world, "EtaRedPhase") {}
    EtaRedPhase(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void rewrite_external(Def*) final;
    const Def* rewrite_no_eta(const Def* def) { return Rewriter::rewrite(def); }
    const Def* rewrite(const Def*);
    const Def* rewrite_imm_Var(const Var*) final;
};

} // namespace mim
