#pragma once

#include <mim/phase.h>

namespace mim::plug::x86 {

/// Writes GNU assembler code (AT&T syntax) for the scheduled world to `<world>.s` (or `a.s` if the world is unnamed).
/// The output path can be overridden via `-X x86:o=<file>` or `-X x86:output=<file>`; `<file>` may be `-` for stdout.
/// Every value lives in its own stack slot, and only `rax` and `rcx` hold values within an instruction.
class Emit : public Phase {
public:
    using Phase::Phase;

    void start() final;
};

} // namespace mim::plug::x86
