#include "mim/plug/x86/x86.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/x86/phase/isel.h"

namespace mim::plug::x86 {

class Emit : public Phase {
public:
    using Phase::Phase;

    void start() override { world().log().d("x86 backend args: {}", fe::Join(args())); }
};

} // namespace mim::plug::x86

using namespace mim;

static void reg_phases(Flags2Phases& phases) {
    Phase::hook<plug::x86::isel, plug::x86::phase::ISel>(phases);
    Phase::hook<plug::x86::emit, plug::x86::Emit>(phases);
}

MIM_PLUGIN_ENTRY(x86) { plugin.register_phases = reg_phases; }
