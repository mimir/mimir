#include "mim/plug/core/core.h"

#include <mim/config.h>
#include <mim/phase.h>

using namespace mim;
using namespace mim::plug;

MIM_PLUGIN_ENTRY(core) { plugin.register_normalizers = core::register_normalizers; }
