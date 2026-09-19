#include "mim/plug/ord/ord.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

MIM_PLUGIN_ENTRY(ord) { return {"ord", MIM_VERSION, plug::ord::register_normalizers, {}, {}, {}, {}, {}, {}, {}}; }
