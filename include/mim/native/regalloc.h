#pragma once

#include <optional>

#include "mim/plugin.h"

#include "mim/native/native.h"

namespace mim::native {

/// Prints the Liveness of every function to `-X <backend>:live=<file>` or stdout; a debugging and testing aid.
class Live : public FunPhase {
public:
    using FunPhase::FunPhase;

private:
    void visit(Lam*) final;
};

/// Spills and reloads until at most `k` values of each register class are live at every point of a scheduled function.
/// This is Belady's MIN per block with global next-use distances (Braun & Hack, CC 2009).
/// A value is spilled right after its definition, and only if it is reloaded somewhere or flows into a memory φ.
/// A parameter that is not in a register at the entry of its block becomes a memory φ of the type Target::spill_type.
/// Reloads give a value several definitions; new block parameters repair SSA form (Braun et al., CC 2013).
/// Critical edges are split first, so coupling code goes to the end of the predecessor.
/// `k` is `-X <backend>:k=<n>` or the number of allocatable registers of a class.
class Spilling : public FunPhase {
public:
    Spilling(World& world, flags_t annex, const Target& target);

private:
    void visit(Lam*) final;

    std::optional<size_t> k_;
};

/// Copies fixed and live tied operands by a jump right before their instruction, keeping the graph chordal (Hack).
/// Likewise, the live fixed results of an instruction go to a fresh continuation right after it.
class Constrain : public FunPhase {
public:
    using FunPhase::FunPhase;

private:
    void visit(Lam*) final;
};

/// Assigns a register to every value by one PBQP per function and register class (Buchwald, Zwinkau, Bersch, CC 2011).
/// The result is in the graph: every use of a value `v` in register `r` becomes Target::in.
/// Expects the output of Spilling and Constrain, so the interference graph is chordal.
/// Removes chain instructions whose results are all dead, so its instruction implies the register of any unused result.
/// A checker verifies the assignment on a fresh Liveness.
class Color : public FunPhase {
public:
    using FunPhase::FunPhase;

private:
    void visit(Lam*) final;
    /// The stream of `-X <backend>:color-dot=<file>`, if requested.
    std::ostream* dot();

    Out dot_;
};

} // namespace mim::native
