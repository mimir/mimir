#pragma once

#include "mim/phase.h"

namespace mim {

/// Specializes a Lam at its call site by inlining all Pi-typed (i.e. higher-order) arguments.
class LamSpec : public RWPhase {
public:
    LamSpec(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    const Def* rewrite_imm_App(const App*) final;

    Def2Def old2new_;
};

} // namespace mim
