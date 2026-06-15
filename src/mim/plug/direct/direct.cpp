#include "mim/plug/direct/direct.h"

#include <mim/plugin.h>

#include "mim/plug/direct/phase/cps2ds.h"
#include "mim/plug/direct/phase/ds2cps.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    Stage::hook<direct::ds2cps_phase, direct::DS2CPS>(stages);
    Stage::hook<direct::cps2ds_phase, direct::CPS2DSPhase>(stages);
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"direct", MIM_VERSION, nullptr, reg_stages}; }
