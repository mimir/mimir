#include "mim/plug/x86_isel/x86_isel.h"

#include <mim/plugin.h>

#include "mim/plug/x86_isel/gen.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::x86_isel::gen, plug::x86_isel::Gen>(phases); }

// clang-format off
static constexpr PluginArg known_args[] = {
    {"o=<file>, output=<file>", "Writes the rule tables of `x86_isel.gen` to `<file>` instead of stdout."},
};
// clang-format on

MIM_PLUGIN_ENTRY(x86_isel) {
    plugin.register_normalizers = plug::x86_isel::register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.args                 = known_args;
    plugin.num_args             = std::size(known_args);
}
