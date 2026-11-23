#include "mim/plug/arm/arm.h"

#include <mim/plugin.h>

#include <mim/native/regalloc.h>

#include "mim/plug/arm/emit.h"
#include "mim/plug/arm/phase/isel.h"
#include "mim/plug/arm/target.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<plug::arm::isel, plug::arm::phase::ISel>(phases);
    const auto& target = plug::arm::target();
    native::hook<plug::arm::spilling, native::Spilling>(phases, target);
    native::hook<plug::arm::color, native::Color>(phases, target);
    native::hook<plug::arm::live, native::Live>(phases, target);
    Phase::hook<plug::arm::emit, plug::arm::Emit>(phases);
}

// clang-format off
static constexpr PluginArg known_args[] = {
    {"o=<file>, output=<file>", "Writes the assembly to `<file>` instead of the default `<world>.s`/`a.s`; `<file>` may be `-` for stdout."},
    {"live=<file>", "Writes the output of `arm.live` to `<file>` instead of stdout."},
    {"k=<n>", "Lets `arm.spilling` keep at most `<n>` values in registers instead of 26."},
    {"color-dot=<file>", "Writes the register-assignment PBQP graph of each function in Graphviz DOT format to `<file>`, with the chosen options highlighted; `<file>` may be `-` for stdout."},
};
// clang-format on

MIM_PLUGIN_ENTRY(arm) {
    plugin.register_normalizers = plug::arm::register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.args                 = known_args;
    plugin.num_args             = std::size(known_args);
    plugin.target               = &plug::arm::target();
}
