#include "mim/plug/x86/x86.h"

#include <mim/plugin.h>

#include "mim/plug/x86/emit.h"
#include "mim/plug/x86/phase/color.h"
#include "mim/plug/x86/phase/constrain.h"
#include "mim/plug/x86/phase/isel.h"
#include "mim/plug/x86/phase/legalize.h"
#include "mim/plug/x86/phase/live.h"
#include "mim/plug/x86/phase/schedule.h"
#include "mim/plug/x86/phase/spilling.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<plug::x86::legalize, plug::x86::phase::Legalize>(phases);
    Phase::hook<plug::x86::isel, plug::x86::phase::ISel>(phases);
    Phase::hook<plug::x86::schedule, plug::x86::phase::Schedule>(phases);
    Phase::hook<plug::x86::spilling, plug::x86::phase::Spilling>(phases);
    Phase::hook<plug::x86::constrain, plug::x86::phase::Constrain>(phases);
    Phase::hook<plug::x86::color, plug::x86::phase::Color>(phases);
    Phase::hook<plug::x86::live, plug::x86::phase::Live>(phases);
    Phase::hook<plug::x86::emit, plug::x86::Emit>(phases);
}

// clang-format off
static constexpr PluginArg known_args[] = {
    {"o=<file>, output=<file>", "Writes the assembly to `<file>` instead of the default `<world>.s`/`a.s`; `<file>` may be `-` for stdout."},
    {"live=<file>", "Writes the output of `x86.live` to `<file>` instead of stdout."},
    {"k=<n>", "Lets `x86.spilling` keep at most `<n>` general-purpose values in registers instead of 14."},
    {"isel-dot=<file>", "Writes the instruction-selection PBQP graph of `x86.isel` in Graphviz DOT format to `<file>`, with the chosen options highlighted; `<file>` may be `-` for stdout."},
    {"color-dot=<file>", "Writes the register-assignment PBQP graph of each function in Graphviz DOT format to `<file>`, with the chosen options highlighted; `<file>` may be `-` for stdout."},
};
// clang-format on

MIM_PLUGIN_ENTRY(x86) {
    plugin.register_normalizers = plug::x86::register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.args                 = known_args;
    plugin.num_args             = std::size(known_args);
}
