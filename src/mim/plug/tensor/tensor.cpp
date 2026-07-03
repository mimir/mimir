#include "mim/plug/tensor/tensor.h"

#include "mim/plugin.h"

#include "mim/plug/tensor/phase/fuse.h"
#include "mim/plug/tensor/phase/lower.h"
#include "mim/plug/tensor/phase/lower_map_reduce.h"

using namespace mim;
using namespace mim::plug;

namespace mim::plug::tensor {
void reg_stages(Flags2Stages& stages) {
    Stage::hook<lower_tensor, phase::Lower>(stages);
    Stage::hook<lower_map_reduce, phase::LowerMapReduce>(stages);
    Stage::hook<fuse_tensor, phase::Fuse>(stages);
}
} // namespace mim::plug::tensor

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"tensor", MIM_VERSION, tensor::register_normalizers, tensor::reg_stages};
}
