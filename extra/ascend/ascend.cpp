#include "mim/plug/ascend/ascend.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "emit.h"

namespace mim::plug::ascend {

namespace {

void register_phases(Flags2Phases& phases) {
    register_hfusion(phases);
    register_hivm(phases);
}

constexpr PluginArg Args[] = {
    {"o=<file>, output=<file>",                                         "Writes emitted MLIR to `<file>`; use `-` for stdout."},
    {             "ub=<bytes>", "Unified-buffer capacity in bytes for `emit_hivm`. The default is 2031616 (Ascend950PR_9579)."},
};

} // namespace

} // namespace mim::plug::ascend

using namespace mim;

extern "C" MIM_EXPORT mim::Plugin mim_get_plugin() {
    return {"ascend",
            MIM_VERSION,
            {},
            mim::plug::ascend::register_phases,
            mim::plug::ascend::Args,
            std::size(mim::plug::ascend::Args),
            {},
            0};
}
