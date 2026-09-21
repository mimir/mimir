#include "mim/plug/nvptx/nvptx.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include <mim/plug/gpu/gpu.h>

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) {
    MIM_REPL(phases, nvptx::stream_impl_repl, {
        auto stream_flags = Annex::base<gpu::Stream>();
        if (def->flags() == stream_flags) return world().annex<nvptx::Stream>();
        return {};
    });
}

MIM_PLUGIN_ENTRY(nvptx) { plugin.register_phases = reg_phases; }
