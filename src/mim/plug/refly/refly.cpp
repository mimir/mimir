#include "mim/plug/refly/refly.h"

#include <mim/config.h>
#include <mim/phase.h>

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    MIM_REPL(phases, refly::remove_dbg_repl, {
        if (auto dbg_perm = Axm::isa(refly::dbg::perm, def)) {
            auto [lvl, x] = dbg_perm->args<2>();
            log().d("dbg perm: {}", x);
            return x;
        }

        return {};
    });
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"refly", MIM_VERSION, refly::register_normalizers, reg_phases, {}, {}, {}, {}};
}
