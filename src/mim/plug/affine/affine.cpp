#include "mim/plug/affine/affine.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/affine/phase/lower_for.h"
#include "mim/plug/affine/phase/lower_index.h"

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<affine::lower_for, affine::phase::LowerFor>(phases);
    Phase::hook<affine::lower_index, affine::phase::LowerIndex>(phases);
}

MIM_PLUGIN_ENTRY(affine) { plugin.register_phases = reg_phases; }
