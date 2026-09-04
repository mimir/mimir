#include "mim/plug/affine/affine.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/affine/phase/lower_for.h"
#include "mim/plug/affine/phase/lower_index.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    Phase::hook<affine::lower_for, affine::phase::LowerFor>(phases);
    Phase::hook<affine::lower_index, affine::phase::LowerIndex>(phases);
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {
        "affine", MIM_VERSION, {}, reg_phases, {}, {}, {}, {},
    };
}
