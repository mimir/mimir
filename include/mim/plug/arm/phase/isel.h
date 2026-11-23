#pragma once

#include <mim/native/isel.h>

namespace mim::plug::arm::phase {

/// Selects AArch64 instructions by the rules of `arm_isel`; see native::ISel.
class ISel : public native::ISel {
public:
    ISel(World& world, flags_t annex);

private:
    /// The emitter materializes any literal operand that its encoding has no immediate for.
    bool fits_imm(const Def*) const final { return true; }
};

} // namespace mim::plug::arm::phase
