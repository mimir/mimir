#pragma once

#include <mim/asm.h>

namespace mim::plug::arm {

/// Writes GNU assembler code (AArch64 syntax) for the scheduled world; see AsmEmit for the output file.
/// Values live in the registers that `arm.color` assigned; jumps, calls, returns, and the entry are parallel copies.
class Emit : public AsmEmit {
public:
    Emit(World& world, flags_t annex)
        : AsmEmit(world, annex, "arm") {}

private:
    void emit(std::ostream& os, Lam* fun, bool global) final;
};

} // namespace mim::plug::arm
