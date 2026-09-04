#include "mim/plug/btensor/btensor.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/btensor/phase/lower_map_reduce.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    Phase::hook<btensor::lower_map_reduce, btensor::phase::LowerMapReduce>(phases);
    // The buffer-to-pointer lowering now lives in the `buffer` plugin (%buffer.lower_ptr).
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"btensor", MIM_VERSION, btensor::register_normalizers, reg_phases, {}, {}, {}, {}};
}
