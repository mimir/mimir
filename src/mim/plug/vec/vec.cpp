#include "mim/plug/vec/vec.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

MIM_PLUGIN_ENTRY(vec) { plugin.register_normalizers = plug::vec::register_normalizers; }
