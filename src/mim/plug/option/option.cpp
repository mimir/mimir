#include "mim/plug/option/option.h"

#include <mim/phase.h>
#include <mim/plugin.h>

using namespace mim;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
MIM_PLUGIN_ENTRY(option) { plugin.register_normalizers = plug::option::register_normalizers; }
