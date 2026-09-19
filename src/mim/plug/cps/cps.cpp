#include "mim/plug/cps/cps.h"

#include <mim/plugin.h>

#include "mim/plug/cps/phase/conv.h"

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) { Phase::hook<cps::conv, cps::Conv>(phases); }

MIM_PLUGIN_ENTRY(cps) { plugin.register_phases = reg_phases; }
