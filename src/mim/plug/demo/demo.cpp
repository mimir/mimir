#include "mim/plug/demo/demo.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
MIM_PLUGIN_ENTRY(demo) { return {"demo", MIM_VERSION, plug::demo::register_normalizers, {}, {}, {}, {}, {}, {}, {}}; }
