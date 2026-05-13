#include <mim/pass.h>
#include <mim/plugin.h>

#include "mim/plug/spirv/autogen.h"
#include "mim/plug/spirv/be/emit.h"

using namespace mim;
using namespace mim::plug;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    // return {"spirv", plug::spirv::register_normalizers, nullptr,
    //     [](Backends& backends) { backends["spirv"] = &spirv::emit_asm; }};
    return {"spirv", MIM_VERSION, nullptr, nullptr, [](Backends& backends) { backends["spirv"] = &spirv::emit_asm; }};
}
