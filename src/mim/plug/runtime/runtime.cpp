#include "mim/plug/runtime/runtime.h"

#include <mim/plugin.h>

using namespace mim;
using namespace mim::plug;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"runtime", MIM_VERSION, runtime::register_normalizers, nullptr};
}
