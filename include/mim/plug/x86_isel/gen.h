#pragma once

#include <mim/phase.h>

namespace mim::plug::x86_isel {

/// Writes the closed rules among the annexes of this plugin as the C++ tables of mim::plug::x86::rules.
class Gen : public Phase {
public:
    Gen(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() final;
};

} // namespace mim::plug::x86_isel
