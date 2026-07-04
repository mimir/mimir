#include "mim/plug/matrix/matrix.h"

#include <mim/plugin.h>
#include <mim/stage.h>

#include "mim/plug/matrix/phase/lower_matrix_highlevel.h"
#include "mim/plug/matrix/phase/lower_matrix_lowlevel.h"
#include "mim/plug/matrix/phase/lower_matrix_mediumlevel.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    // clang-format off
    Stage::hook<matrix::lower_matrix_high_level_map_reduce, matrix::LowerMatrixHighLevelMapRed>(stages);
    Stage::hook<matrix::lower_matrix_medium_level,          matrix::LowerMatrixMediumLevel    >(stages);
    Stage::hook<matrix::lower_matrix_low_level,             matrix::LowerMatrixLowLevel       >(stages);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"matrix", MIM_VERSION, matrix::register_normalizers, reg_stages};
}
