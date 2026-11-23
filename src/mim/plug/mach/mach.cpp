#include "mim/plug/mach/mach.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/mach/emit.h"
#include "mim/plug/mach/gen.h"
#include "mim/plug/mach/isel.h"
#include "mim/plug/mach/legalize.h"
#include "mim/plug/mach/regalloc.h"
#include "mim/plug/mach/schedule.h"

using namespace mim;
using namespace plug::mach;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<legalize, Legalize>(phases);
    hook<schedule, Schedule>(phases);
    hook<spilling, Spilling>(phases);
    hook<constrain, Constrain>(phases);
    hook<color, Color>(phases);
    hook<live, Live>(phases);
}

// MIM_PLUGIN_SYM stringifies its argument, so the name must stay unqualified.
static const PluginSym known_syms[] = {
    MIM_PLUGIN_SYM(mim_mach_gen),
    MIM_PLUGIN_SYM(mim_mach_emit),
    MIM_PLUGIN_SYM(mim_mach_isel),
};

MIM_PLUGIN_ENTRY(mach) {
    plugin.register_normalizers = register_normalizers;
    plugin.register_phases      = reg_phases;
    plugin.syms                 = known_syms;
    plugin.num_syms             = std::size(known_syms);
}
