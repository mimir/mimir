#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Copies fixed and live tied operands by a jump right before their instruction, keeping the graph chordal (Hack).
class Constrain : public ClosedMutPhase<Lam> {
public:
    Constrain(World& world, flags_t annex)
        : ClosedMutPhase(world, annex, true) {}

private:
    void visit(Lam*) final;
};

} // namespace mim::plug::x86::phase
