#include "mim/plug/sflow/sflow.h"

#include <mim/pass.h>
#include <mim/plugin.h>

using namespace mim;

/// Registers normalizers as well as Phase%s and Pass%es for the Axm%s of this Plugin.
extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"sflow", plug::sflow::register_normalizers, nullptr, nullptr}; }
