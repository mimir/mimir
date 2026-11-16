#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Assigns a general-purpose register to every value by one PBQP per function (Buchwald, Zwinkau, Bersch, CC 2011).
/// The result is in the graph: every use of a value `v` in register `r` becomes `x86.in r v`.
/// Expects the output of x86.spilling and x86.constrain, so the interference graph is chordal.
/// Removes chain instructions whose results are all dead, so its instruction implies the register of any unused result.
class Color : public ClosedMutPhase<Lam> {
public:
    Color(World& world, flags_t annex)
        : ClosedMutPhase(world, annex, true) {}

private:
    void visit(Lam*) final;
};

} // namespace mim::plug::x86::phase
