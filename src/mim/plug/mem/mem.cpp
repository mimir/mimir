#include "mim/plug/mem/mem.h"

#include <mim/config.h>
#include <mim/phase.h>

#include "mim/plug/mem/mem.h"
#include "mim/plug/mem/phase/add_mem.h"
#include "mim/plug/mem/phase/reshape.h"
#include "mim/plug/mem/phase/seo.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) {
    MIM_REPL(phases, mem::remem_repl, {
        if (auto remem = Axm::isa<mem::remem>(def)) return remem->arg();
        return {};
    });

    MIM_REPL(phases, mem::alloc2malloc_repl, {
        if (auto alloc = Axm::isa<mem::alloc>(def)) {
            auto [pointee, addr_space] = alloc->decurry()->args<2>();
            return mem::op_malloc(pointee, addr_space, alloc->arg());
        } else if (auto slot = Axm::isa<mem::slot>(def)) {
            auto [Ta, mi]              = slot->uncurry_args<2>();
            auto [pointee, addr_space] = Ta->projs<2>();
            auto [mem, id]             = mi->projs<2>();
            return mem::op_mslot(pointee, addr_space, mem, id);
        }
        if (auto remem = Axm::isa<mem::remem>(def)) return remem->arg();
        return {};
    });

    // clang-format off
    Phase::hook<mem::add_mem,  mem::phase::AddMem >(phases);
    Phase::hook<mem::seo,      mem::phase::SEO    >(phases);
    Phase::hook<mem::reshape,  mem::phase::Reshape>(phases);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"mem", MIM_VERSION, mem::register_normalizers, reg_phases}; }
