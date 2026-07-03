#include "mim/plug/cps/cps.h"

#include <mim/plugin.h>

#include "mim/plug/cps/phase/conv.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) { Stage::hook<cps::conv, cps::Conv>(stages); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"cps", MIM_VERSION, nullptr, reg_stages}; }
