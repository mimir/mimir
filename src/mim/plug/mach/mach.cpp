#include "mim/plug/mach/mach.h"

#include <mim/plugin.h>

#include <mim/native/regalloc.h>

#include "mim/plug/mach/gen.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    native::hook<plug::mach::spilling, native::Spilling>(phases);
    native::hook<plug::mach::constrain, native::Constrain>(phases);
    native::hook<plug::mach::color, native::Color>(phases);
    native::hook<plug::mach::live, native::Live>(phases);
}

// MIM_PLUGIN_SYM stringifies its argument, so the name must stay unqualified.
using namespace plug::mach;
static const PluginSym known_syms[] = {MIM_PLUGIN_SYM(mim_mach_gen)};

MIM_PLUGIN_ENTRY(mach) {
    plugin.register_normalizers = register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.syms                 = known_syms;
    plugin.num_syms             = std::size(known_syms);
}
