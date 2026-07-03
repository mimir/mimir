#include "mim/plug/refly/refly.h"

#include <mim/config.h>
#include <mim/phase.h>

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    MIM_REPL(stages, refly::remove_dbg_repl, {
        if (auto dbg_perm = Axm::isa(refly::dbg::perm, def)) {
            auto [lvl, x] = dbg_perm->args<2>();
            DLOG("dbg_perm: {}", x);
            return x;
        }

        return {};
    });
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"refly", MIM_VERSION, refly::register_normalizers, reg_stages};
}
