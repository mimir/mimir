#include "mim/plug/matrix/matrix.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/matrix/phase/lower_matrix_highlevel.h"
#include "mim/plug/matrix/phase/lower_matrix_mediumlevel.h"
#include "mim/plug/matrix/phase/lower_aff.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    // clang-format off
    Phase::hook<matrix::lower_matrix_high_level_map_reduce, matrix::LowerMatrixHighLevelMapRed>(phases);
    Phase::hook<matrix::lower_matrix_medium_level,          matrix::LowerMatrixMediumLevel    >(phases);
    Phase::hook<matrix::lower_aff,                          matrix::LowerAff                  >(phases);
    // clang-format on
    // The matrix-to-pointer lowering now lives in the `buffer` plugin (%buffer.lower_ptr).
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"matrix", MIM_VERSION, matrix::register_normalizers, reg_phases};
}
