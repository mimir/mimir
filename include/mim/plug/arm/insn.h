#pragma once

#include <mim/axm.h>
#include <mim/lam.h>

#include "mim/plug/arm/autogen.h"

/// Machine facts about the instructions of the `arm` plugin, shared by `arm_isel.gen` and the backend.
namespace mim::plug::arm::insn {

/// The latency in cycles of the instruction @p base, roughly as on recent cores.
inline u32 latency(flags_t base) {
    if (base == Annex::Base<mul> || base == Annex::Base<madd>) return 3;
    if (base == Annex::Base<udiv> || base == Annex::Base<sdiv>) return 12;
    if (base == Annex::Base<load>) return 4;
    return 1;
}

/// The cost of the instruction @p app: its latency plus one, so of two equally fast covers the shorter wins.
inline u32 cost(const App* app) { return latency(app->axm()->base()) + 1; }

} // namespace mim::plug::arm::insn
