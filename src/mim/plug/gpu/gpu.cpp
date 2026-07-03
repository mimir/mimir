#include "mim/plug/gpu/gpu.h"

#include <mim/config.h>
#include <mim/driver.h>
#include <mim/pass.h>
#include <mim/phase.h>
#include <mim/plugin.h>

#include <mim/plug/gpu/phase/mem_checks.h>
#include <mim/plug/gpu/phase/remove_double_syncs.h>
#include <mim/plug/mem/mem.h>

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    MIM_REPL(stages, gpu::check_addr_spaces_repl, {
        auto global_as = Lit::as(world().annex<gpu::addr_space_global>());
        auto shared_as = Lit::as(world().annex<gpu::addr_space_shared>());
        auto const_as  = Lit::as(world().annex<gpu::addr_space_const>());
        auto local_as  = Lit::as(world().annex<gpu::addr_space_local>());
        if (auto malloc = Axm::isa<mem::malloc>(def)) {
            auto addr_space = Lit::as(malloc->decurry()->arg(1));
            if (addr_space == shared_as || addr_space == const_as || addr_space == local_as)
                error("Invalid use of %mem.malloc: cannot be used in address space {}: {}", addr_space, malloc);
        } else if (auto free = Axm::isa<mem::free>(def)) {
            auto addr_space = Lit::as(free->decurry()->arg(1));
            if (addr_space == shared_as || addr_space == const_as || addr_space == local_as)
                error("Invalid use of %mem.free: cannot be used in address space {}: {}", addr_space, free);
        } else if (auto mslot = Axm::isa<mem::mslot>(def)) {
            auto addr_space = Lit::as(mslot->decurry()->arg(1));
            if (addr_space == global_as || addr_space == const_as)
                error("Invalid use of %mem.mslot: cannot be used in address space {}: {}", addr_space, mslot);
        } else if (auto store = Axm::isa<mem::store>(def)) {
            auto addr_space = Lit::as(store->decurry()->arg(1));
            if (addr_space == const_as)
                error("Invalid use of %mem.store: cannot be used in address space {}: {}", addr_space, store);
        }
        return {};
    });

    MIM_REPL(stages, gpu::host_malloc2gpualloc_repl, {
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
    Stage::hook<gpu::mem_checks_phase,                 gpu::phase::MemChecks        >(stages);
    Stage::hook<gpu::remove_double_syncs,              gpu::phase::RemoveDoubleSyncs>(stages);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"gpu", MIM_VERSION, nullptr, reg_stages}; }
