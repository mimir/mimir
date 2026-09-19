#include "mim/plug/ord/ord.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

MIM_PLUGIN_ENTRY(ord) { plugin.register_normalizers = plug::ord::register_normalizers; }
