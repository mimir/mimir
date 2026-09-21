#include "mim/plug/tuple/tuple.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

MIM_PLUGIN_ENTRY(tuple) { plugin.register_normalizers = plug::tuple::register_normalizers; }
