#pragma once

#include <mim/phase.h>

namespace mim::plug::x86 {

/// Writes GNU assembler code (AT&T syntax) for the scheduled world to `<world>.s` (or `a.s` if the world is unnamed).
/// The output path can be overridden via `-X x86:o=<file>` or `-X x86:output=<file>`; `<file>` may be `-` for stdout.
/// Values live in the registers that x86.color assigned; jumps, calls, returns, and the entry are parallel copies.
class Emit : public Phase {
public:
    using Phase::Phase;

    void start() final;
};

} // namespace mim::plug::x86
