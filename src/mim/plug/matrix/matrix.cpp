#include "mim/plug/matrix/matrix.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/matrix/phase/lower.h"
#include "mim/plug/matrix/phase/lower_map_reduce.h"
#include "mim/plug/matrix/phase/lower_map_reduce_idx.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    // clang-format off
    Phase::hook<matrix::lower_matrix,         matrix::phase::Lower            >(phases);
    Phase::hook<matrix::lower_map_reduce_idx, matrix::phase::LowerMapReduceIdx>(phases);
    Phase::hook<matrix::lower_map_reduce,     matrix::phase::LowerMapReduce   >(phases);
    // clang-format on
    // The matrix-to-pointer lowering now lives in the `buffer` plugin (%buffer.lower_ptr).
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"matrix", MIM_VERSION, matrix::register_normalizers, reg_phases};
}
