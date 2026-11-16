#include "mim/plug/x86/x86.h"

#include <mim/plugin.h>

#include "mim/plug/x86/emit.h"
#include "mim/plug/x86/phase/isel.h"
#include "mim/plug/x86/phase/live.h"
#include "mim/plug/x86/phase/schedule.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<plug::x86::isel, plug::x86::phase::ISel>(phases);
    Phase::hook<plug::x86::schedule, plug::x86::phase::Schedule>(phases);
    Phase::hook<plug::x86::live, plug::x86::phase::Live>(phases);
    Phase::hook<plug::x86::emit, plug::x86::Emit>(phases);
}

// clang-format off
static constexpr PluginArg known_args[] = {
    {"o=<file>, output=<file>", "Writes the assembly to `<file>` instead of the default `<world>.s`/`a.s`; `<file>` may be `-` for stdout."},
    {"live=<file>", "Writes the output of `x86.live` to `<file>` instead of stdout."},
};
// clang-format on

MIM_PLUGIN_ENTRY(x86) {
    plugin.register_phases = reg_phases;
    plugin.args            = known_args;
    plugin.num_args        = std::size(known_args);
}
