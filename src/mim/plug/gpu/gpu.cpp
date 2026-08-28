#include "mim/plug/gpu/gpu.h"

#include <mim/config.h>
#include <mim/driver.h>
#include <mim/phase.h>
#include <mim/plugin.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/gpu/phase/lower_map_reduce.h"
#include "mim/plug/gpu/phase/mem_checks.h"
#include "mim/plug/gpu/phase/merge_init_deinit.h"
#include "mim/plug/gpu/phase/remove_double_syncs.h"
#include "mim/plug/gpu/phase/split_apply.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    MIM_REPL(phases, gpu::check_addr_spaces_repl, {
        auto global_as = Lit::as(world().annex<gpu::addr_space_global>());
        auto shared_as = Lit::as(world().annex<gpu::addr_space_shared>());
        auto const_as  = Lit::as(world().annex<gpu::addr_space_const>());
        auto local_as  = Lit::as(world().annex<gpu::addr_space_local>());
        if (auto malloc = Axm::isa<mem::malloc>(def)) {
            auto addr_space = Lit::as(malloc->decurry()->arg(1));
            if (addr_space == shared_as || addr_space == const_as || addr_space == local_as)
                fe::throwf("`%mem.malloc` cannot be used in address space {}: `{}`", addr_space, malloc);
        } else if (auto free = Axm::isa<mem::free>(def)) {
            auto addr_space = Lit::as(free->decurry()->arg(1));
            if (addr_space == shared_as || addr_space == const_as || addr_space == local_as)
                fe::throwf("`%mem.free` cannot be used in address space {}: `{}`", addr_space, free);
        } else if (auto mslot = Axm::isa<mem::mslot>(def)) {
            auto addr_space = Lit::as(mslot->decurry()->arg(1));
            if (addr_space == global_as || addr_space == const_as)
                fe::throwf("`%mem.mslot` cannot be used in address space {}: `{}`", addr_space, mslot);
        } else if (auto store = Axm::isa<mem::store>(def)) {
            auto addr_space = Lit::as(store->decurry()->arg(1));
            if (addr_space == const_as)
                fe::throwf("`%mem.store` cannot be used in address space {}: `{}`", addr_space, store);
        }
        return {};
    });

    MIM_REPL(phases, gpu::host_malloc2gpualloc_repl, {
        auto global_as = Lit::as(world().annex<gpu::addr_space_global>());
        if (auto malloc = Axm::isa<mem::malloc>(def)) {
            auto [type, addr_space] = malloc->decurry()->args<2>();
            if (Lit::as(addr_space) == global_as) {
                auto [mem, _] = malloc->args<2>();
                World& w      = type->world();
                return w.app(w.app(w.annex<gpu::alloc>(gpu::alloc::block), type), mem);
            }
        } else if (auto free = Axm::isa<mem::free>(def)) {
            auto [type, addr_space] = free->decurry()->args<2>();
            if (Lit::as(addr_space) == global_as) {
                auto [mem, ptr] = free->args<2>();
                World& w        = type->world();
                return w.app(w.app(w.annex<gpu::free>(gpu::free::block), type), {mem, ptr});
            }
        }
        return {};
    });

    // clang-format off
    Phase::hook<gpu::mem_checks,                gpu::phase::MemChecks        >(phases);
    Phase::hook<gpu::remove_double_syncs,       gpu::phase::RemoveDoubleSyncs>(phases);
    Phase::hook<gpu::split_apply,               gpu::phase::SplitApply       >(phases);
    Phase::hook<gpu::lower_btensor_map_reduce,  gpu::phase::LowerMapReduce   >(phases);
    Phase::hook<gpu::merge_init_deinit,         gpu::phase::MergeInitDeinit  >(phases);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"gpu", MIM_VERSION, nullptr, reg_phases}; }
