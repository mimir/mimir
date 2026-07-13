#include "mim/plug/gpu/phase/remove_double_syncs.h"

#include <mim/axm.h>

#include <mim/plug/mem/mem.h>

namespace mim::plug::gpu::phase {

const Def* RemoveDoubleSyncs::rewrite_imm_App(const App* app) {
    if (auto sync_work_items = Axm::isa<gpu::sync_work_items>(app)) {
        auto [m1, m3]   = sync_work_items->args<2>();
        auto m1_extract = m1->isa<Extract>();
        auto m3_extract = m3->isa<Extract>();
        if (!m1_extract || !m3_extract || m1_extract->tuple() != m3_extract->tuple())
            return Super::rewrite_imm_App(app);
        auto common_prev          = m1_extract->tuple();
        bool has_common_prev_sync = Axm::isa<gpu::sync_work_items>(common_prev);
        auto common_var           = common_prev->isa<Var>();
        bool is_kernel_start      = common_var ? common_var->mut()->is_external() : false;
        if (has_common_prev_sync || is_kernel_start) {
            auto new_arg = rewrite(sync_work_items->arg());
            map(app, new_arg);
            return new_arg;
        }
    }
    return Super::rewrite_imm_App(app);
}

} // namespace mim::plug::gpu::phase
