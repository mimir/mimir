#pragma once

#include <map>

#include <mim/asm.h>

#include "mim/plug/x86/target.h"

namespace mim::plug::x86 {

/// Writes GNU assembler code (AT&T syntax) for the scheduled world; see AsmEmit for the output file.
/// Values live in the registers that `mach.color` assigned; jumps, calls, returns, and the entry are parallel copies.
class Emit : public AsmEmit {
public:
    Emit(World& world, flags_t annex)
        : AsmEmit(world, annex, target()) {}

    /// The index of the 16-byte constant (@p lo, @p hi) in the `.rodata` pool; allocated on first use.
    size_t pool(u64 lo, u64 hi) { return pool_.emplace(std::pair(lo, hi), pool_.size()).first->second; }

private:
    void emit(std::ostream& os, Lam* fun, bool global) final;
    void finish(std::ostream& os) final;

    std::map<std::pair<u64, u64>, size_t> pool_;
};

} // namespace mim::plug::x86
