#include "mim/plug/vec/vec.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"vec", MIM_VERSION, plug::vec::register_normalizers, {}, {}, {}, {}, {}};
}
