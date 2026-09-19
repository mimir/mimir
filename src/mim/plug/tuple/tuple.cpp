#include "mim/plug/tuple/tuple.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

MIM_PLUGIN_ENTRY(tuple) {
    return {"tuple", MIM_VERSION, plug::tuple::register_normalizers, {}, {}, {}, {}, {}, {}, {}};
}
