#include "mim/plug/math/math.h"

#include <mim/config.h>
#include <mim/phase.h>

using namespace mim;

MIM_PLUGIN_ENTRY(math) { return {"math", MIM_VERSION, plug::math::register_normalizers, {}, {}, {}, {}, {}, {}, {}}; }
