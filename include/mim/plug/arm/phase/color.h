#pragma once

#include <mim/phase.h>

namespace mim::plug::arm::phase {

/// Assigns a general-purpose register to every value by greedy graph coloring.
/// The result is in the graph: every use of a value `v` in register `r` becomes `arm.in r v`.
/// Jumps, calls, and returns are parallel copies; values that live across a call stay in callee-saved registers.
class Color : public ClosedMutPhase<Lam> {
public:
    Color(World& world, flags_t annex)
        : ClosedMutPhase(world, annex, true) {}

private:
    void visit(Lam*) final;
};

} // namespace mim::plug::arm::phase
