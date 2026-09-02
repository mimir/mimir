#include "mim/plug/ord/ord.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"ord", MIM_VERSION, plug::ord::register_normalizers, {}, {}, {}};
}
