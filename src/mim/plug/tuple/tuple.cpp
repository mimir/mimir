#include "mim/plug/tuple/tuple.h"

#include <mim/plugin.h>
#include <mim/stage.h>

using namespace mim;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"tuple", MIM_VERSION, plug::tuple::register_normalizers, nullptr};
}
