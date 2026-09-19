#include "mim/plug/refly/refly.h"

#include <mim/config.h>
#include <mim/phase.h>

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) {
    MIM_REPL(phases, refly::remove_dbg_repl, {
        if (auto dbg_perm = Axm::isa(refly::dbg::perm, def)) {
            auto [lvl, x] = dbg_perm->args<2>();
            log().d("dbg perm: {}", x);
            return x;
        }

        return {};
    });
}

MIM_PLUGIN_ENTRY(refly) {
    plugin.register_normalizers = refly::register_normalizers;
    plugin.register_phases      = reg_phases;
}
