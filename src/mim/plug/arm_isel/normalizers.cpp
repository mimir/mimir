#include <mim/world.h>

#include "mim/plug/arm_isel/arm_isel.h"

namespace mim::plug::arm_isel {

const Def* normalize_lit(const Def* type, const Def*, const Def* arg) {
    return Lit::isa(arg) ? type->world().lit_tt() : nullptr;
}

MIM_arm_isel_NORMALIZER_IMPL

} // namespace mim::plug::arm_isel
