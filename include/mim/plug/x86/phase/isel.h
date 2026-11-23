#pragma once

#include <mim/native/isel.h>

namespace mim::plug::x86::phase {

/// Selects x86 instructions by the rules of `x86_isel`; see native::ISel.
class ISel : public native::ISel {
public:
    ISel(World& world, flags_t annex);

private:
    /// A literal that fits a sign-extended 32-bit immediate.
    bool fits_imm(const Def* lit) const final;
};

} // namespace mim::plug::x86::phase
