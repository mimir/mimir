#include "mim/plug/omp/omp.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/omp/phase/outline.h"

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) { Phase::hook<omp::outline, omp::phase::Outline>(phases); }

MIM_PLUGIN_ENTRY(omp) { plugin.register_phases = reg_phases; }
