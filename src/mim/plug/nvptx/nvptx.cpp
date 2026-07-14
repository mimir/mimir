#include "mim/plug/nvptx/nvptx.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/gpu/gpu.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    MIM_REPL(stages, nvptx::stream_impl_repl, {
        auto stream_flags = Annex::base<gpu::Stream>();
        if (def->flags() == stream_flags) return world().annex<nvptx::Stream>();
        return {};
    });
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"nvptx", MIM_VERSION, nullptr, reg_stages}; }
