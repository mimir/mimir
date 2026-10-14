#pragma once

#include "mim/phase.h"

namespace mim {

/// Specializes a Lam at its call site by inlining all Pi-typed (i.e. higher-order) arguments.
class LamSpec : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim
