#include "mim/plug/x86/x86.h"

#include <mim/plugin.h>

#include "mim/plug/x86/emit.h"
#include "mim/plug/x86/phase/isel.h"
#include "mim/plug/x86/phase/schedule.h"

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<plug::x86::isel, plug::x86::phase::ISel>(phases);
    Phase::hook<plug::x86::schedule, plug::x86::phase::Schedule>(phases);
    Phase::hook<plug::x86::emit, plug::x86::Emit>(phases);
}

MIM_PLUGIN_ENTRY(x86) { plugin.register_phases = reg_phases; }
