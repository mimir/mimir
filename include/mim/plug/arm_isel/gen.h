#pragma once

#include <mim/native/gen.h>

namespace mim::plug::arm_isel {

/// Writes the closed rules among the annexes of this plugin as the C++ matchers and builders of mim::plug::arm::rules.
class Gen : public native::Gen {
public:
    Gen(World& world, flags_t annex);

    u32 cost(const App* insn) const final;
};

} // namespace mim::plug::arm_isel
