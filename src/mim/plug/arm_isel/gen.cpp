#include "mim/plug/arm_isel/gen.h"

#include <mim/plug/arm/autogen.h>
#include <mim/plug/arm/insn.h>
#include <mim/plug/mach/mach.h>

#include "mim/plug/arm_isel/arm_isel.h"

namespace mim::plug::arm_isel {

Gen::Gen(World& world, flags_t annex)
    : mach::Gen(world,
                annex,
                "arm",
                arm::Plugin_Id,
                "arm_isel",
                Plugin_Id,
                {
                    .reg      = Annex::base<reg>(),
                    .lit      = Annex::base<mach::lit>(),
                    .commutes = Annex::base<commutes>(),
                    .formats  = Annex::base<formats>(),
                    .sizes    = Annex::base<sizes>(),
                }) {}

u32 Gen::cost(const App* insn) const { return arm::insn::cost(insn); }

} // namespace mim::plug::arm_isel
