#include "mim/plug/torch/torch.h"

#include <mim/plugin.h>

#include "mim/plug/torch/phase/lower.h"

using namespace mim;

namespace mim::plug::torch {

void reg_phases(Flags2Phases& phases) {
    Phase::hook<decompose_torch, phase::Decompose>(phases);
    Phase::hook<lower_torch,     phase::Lower    >(phases);
}

} // namespace mim::plug::torch

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"torch", MIM_VERSION, mim::plug::torch::register_normalizers,
            mim::plug::torch::reg_phases};
}
