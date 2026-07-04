#include "mim/plug/option/option.h"

#include <mim/plugin.h>
#include <mim/stage.h>

using namespace mim;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"option", MIM_VERSION, plug::option::register_normalizers, nullptr};
}
