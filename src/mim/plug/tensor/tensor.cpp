#include "mim/plug/tensor/tensor.h"

#include <mim/pass.h>
#include <mim/plugin.h>

using namespace mim;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"tensor", MIM_VERSION, plug::tensor::register_normalizers, nullptr, nullptr};
}
