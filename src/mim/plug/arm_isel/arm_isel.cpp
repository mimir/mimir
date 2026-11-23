#include "mim/plug/arm_isel/arm_isel.h"

#include <mim/plugin.h>

#include "mim/plug/arm_isel/gen.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::arm_isel::gen, plug::arm_isel::Gen>(phases); }

// clang-format off
static constexpr PluginArg known_args[] = {
    {"o=<file>, output=<file>", "Writes the C++ rules of `arm_isel.gen` to `<file>` instead of stdout."},
};
// clang-format on

MIM_PLUGIN_ENTRY(arm_isel) {
    plugin.register_normalizers = plug::arm_isel::register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.args                 = known_args;
    plugin.num_args             = std::size(known_args);
}
