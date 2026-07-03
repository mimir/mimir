#include "mim/plug/affine/affine.h"

#include <mim/config.h>
#include <mim/pass.h>

#include "mim/plug/affine/phase/lower_for.h"
#include "mim/plug/affine/phase/lower_index.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    Stage::hook<affine::lower_for_phase, affine::phase::LowerFor>(stages);
    Stage::hook<affine::lower_index_phase, affine::phase::LowerIndex>(stages);
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"affine", MIM_VERSION, nullptr, reg_stages}; }
