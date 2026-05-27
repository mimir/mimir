#include "mim/plug/cps/cps.h"

#include <mim/plugin.h>

#include "mim/plug/cps/phase/cps2ds.h"
#include "mim/plug/cps/phase/ds2cps.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    Stage::hook<cps::ds2cps_phase, cps::DS2CPS>(stages);
    Stage::hook<cps::cps2ds_phase, cps::CPS2DSPhase>(stages);
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"cps", MIM_VERSION, nullptr, reg_stages, nullptr}; }
