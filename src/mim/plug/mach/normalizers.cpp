#include <mim/world.h>

#include "mim/plug/mach/mach.h"

namespace mim::plug::mach {

const Def* normalize_lit(const Def* type, const Def*, const Def* arg) {
    return Lit::isa(arg) ? type->world().lit_tt() : nullptr;
}

MIM_mach_NORMALIZER_IMPL

} // namespace mim::plug::mach
