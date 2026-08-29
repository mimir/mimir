#include "mim/plug/gpu/phase/merge_init_deinit.h"

#include <mim/plugin.h>
#include <mim/tuple.h>

#include <mim/plug/mem/mem.h>

namespace mim::plug::gpu::phase {

namespace {

constexpr int Max_Hops = 128;

std::optional<nat_t> mem_addr_space(const Def* def) {
    if (auto ty = mem::isa_mem(def))
        if (auto a = Lit::isa<nat_t>(ty->arg())) return a;
    return {};
}

bool is_touching_addr_space(const Def* def, nat_t space) {
    if (auto a = mem_addr_space(def)) return *a == space;
    if (def->type()->isa<Arr>()) return false;
    if (def->num_projs() > 1)
        for (auto proj : def->projs())
            if (is_touching_addr_space(proj, space)) return true;
    return false;
}

} // namespace

const Def* MergeInitDeinit::rewrite_imm_App(const App* app) {
    if (!Axm::isa<gpu::init>(app)) return Super::rewrite_imm_App(app);

    auto& w         = new_world();
    auto gpu_plugin = Annex::mangle(w.sym("gpu"));
    auto global_as  = Lit::as(w.annex<gpu::addr_space_global>());
    auto const_as   = Lit::as(w.annex<gpu::addr_space_const>());

    const Def* cur          = app->arg();
    const App* found_deinit = nullptr;
    // TODO: find and merge more complex combinations of init and deinit occurences
    for (int hop = 0; hop != Max_Hops; ++hop) {
        auto producer = cur->isa<Extract>() ? cur->as<Extract>()->tuple() : cur;

        if (auto deinit = Axm::isa<gpu::deinit>(producer)) {
            found_deinit = deinit;
            break;
        }

        auto [axm, curry, trip] = Axm::get(producer);
        if (!axm) break;
        if (gpu_plugin && axm->plugin() == *gpu_plugin) break;

        auto producer_arg = producer->as<App>()->arg();
        if (is_touching_addr_space(producer_arg, global_as) || is_touching_addr_space(producer_arg, const_as)) break;

        auto next = mem::mem_def(producer_arg);
        if (!next || mem_addr_space(next) != nat_t(0)) break;

        cur = next;
    }

    if (!found_deinit) return Super::rewrite_imm_App(app);

    auto rewritten_deinit_arg                      = rewrite(found_deinit->arg());
    auto [deinit_mem, deinit_global, deinit_const] = rewritten_deinit_arg->projs<3>();

    push();
    map(found_deinit, deinit_mem); // scoped: other uses of found_deinit must still see the real deinit
    auto new_mem = rewrite(app->arg());
    pop();

    return w.tuple({new_mem, deinit_global, deinit_const});
}

} // namespace mim::plug::gpu::phase
