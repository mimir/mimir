#include "mim/plug/cps/cps.h"

#include <mim/plugin.h>

#include "mim/plug/cps/phase/conv.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) { Phase::hook<cps::conv, cps::Conv>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"cps", MIM_VERSION, {}, reg_phases, {}, {}}; }
