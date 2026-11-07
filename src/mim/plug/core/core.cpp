#include "mim/plug/core/core.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/core/phase/legalize_idx.h"
#include "mim/plug/core/phase/lower_variant.h"

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<core::legalize_idx, core::phase::LegalizeIdx>(phases);
    Phase::hook<core::lower_variant, core::phase::LowerVariant>(phases);
}

MIM_PLUGIN_ENTRY(core) {
    plugin.register_normalizers = core::register_normalizers;
    plugin.register_phases      = reg_phases;
}
