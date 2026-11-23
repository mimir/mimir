#pragma once

#include <mim/plug/mach/asm.h>

#include "mim/plug/arm/target.h"

namespace mim::plug::arm {

/// Writes GNU assembler code (AArch64 syntax) for the scheduled world; see AsmEmit for the output file.
/// Values live in the registers that `mach.color` assigned; jumps, calls, returns, and the entry are parallel copies.
class Emit : public mach::AsmEmit {
public:
    Emit(World& world, flags_t annex)
        : mach::AsmEmit(world, annex, target()) {}

private:
    void emit(std::ostream& os, Lam* fun, bool global) final;
};

} // namespace mim::plug::arm
