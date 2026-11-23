#include "mim/plug/arm/phase/isel.h"

#include "mim/plug/arm/isel_rules.h"
#include "mim/plug/arm/target.h"

namespace mim::plug::arm::phase {

ISel::ISel(World& world, flags_t annex)
    : native::ISel(world, annex, target(), rules::table) {}

} // namespace mim::plug::arm::phase
