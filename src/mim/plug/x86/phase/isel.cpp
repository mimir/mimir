#include "mim/plug/x86/phase/isel.h"

#include "mim/plug/x86/imm.h"
#include "mim/plug/x86/isel_rules.h"
#include "mim/plug/x86/target.h"

namespace mim::plug::x86::phase {

ISel::ISel(World& world, flags_t annex)
    : native::ISel(world, annex, target(), rules::table) {}

bool ISel::fits_imm(const Def* lit) const { return x86::fits_imm(lit); }

} // namespace mim::plug::x86::phase
