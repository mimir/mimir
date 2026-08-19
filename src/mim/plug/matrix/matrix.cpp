#include "mim/plug/matrix/matrix.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/matrix/phase/lower_map_reduce.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    Phase::hook<matrix::lower_map_reduce, matrix::phase::LowerMapReduce>(phases);
    // The matrix-to-pointer lowering now lives in the `buffer` plugin (%buffer.lower_ptr).
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"matrix", MIM_VERSION, nullptr, reg_phases}; }
