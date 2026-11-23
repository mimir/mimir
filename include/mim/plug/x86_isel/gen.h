#pragma once

#include <mim/native/gen.h>

namespace mim::plug::x86_isel {

/// Writes the closed rules among the annexes of this plugin as the C++ matchers and builders of mim::plug::x86::rules.
class Gen : public native::Gen {
public:
    Gen(World& world, flags_t annex);

    u32 cost(const App* insn) const final;
    std::optional<size_t> tied(const App* insn) const final;
    std::optional<native::rules::Want> classify(const Def* type) const final;
};

} // namespace mim::plug::x86_isel
