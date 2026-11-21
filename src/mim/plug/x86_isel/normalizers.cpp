#include <mim/world.h>

#include <mim/plug/x86/imm.h>

#include "mim/plug/x86_isel/x86_isel.h"

namespace mim::plug::x86_isel {

const Def* normalize_imm(const Def* type, const Def*, const Def* arg) {
    return Lit::isa(arg) ? type->world().lit_bool(x86::fits_imm(arg)) : nullptr;
}

const Def* normalize_lit(const Def* type, const Def*, const Def* arg) {
    return Lit::isa(arg) ? type->world().lit_tt() : nullptr;
}

MIM_x86_isel_NORMALIZER_IMPL

} // namespace mim::plug::x86_isel
