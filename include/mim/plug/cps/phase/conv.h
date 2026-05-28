#pragma once

#include <mim/phase.h>
#include <mim/schedule.h>

namespace mim::plug::cps {

/// CPS conversion.
class Conv : public RWPhase {
public:
    Conv(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_mut_Lam(Lam*) final;
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::cps
