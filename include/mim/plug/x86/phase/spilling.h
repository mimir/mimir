#pragma once

#include <mim/phase.h>

namespace mim::plug::x86::phase {

/// Spills and reloads until at most `k` values of each register class are live at every point of a scheduled function.
/// This is Belady's MIN per block with global next-use distances (Braun & Hack, CC 2009).
/// A value is spilled right after its definition, and only if it is reloaded somewhere or flows into a memory φ.
/// A parameter that is not in a register at the entry of its block becomes a memory φ of type `x86.Spill s`/`x86.FSpill
/// pe`. Reloads give a value several definitions; new block parameters repair SSA form (Braun et al., CC 2013).
/// Critical edges are split first, so coupling code goes to the end of the predecessor.
/// Every XMM register is caller-saved, so a floating-point value never stays in one across a call.
class Spilling : public ClosedMutPhase<Lam> {
public:
    Spilling(World& world, flags_t annex);

private:
    void visit(Lam*) final;

    std::optional<size_t> k_;
};

} // namespace mim::plug::x86::phase
