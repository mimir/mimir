#include "mim/plug/mem/mem.h"

#include <mim/config.h>
#include <mim/pass.h>

#include <mim/pass/beta_red.h>
#include <mim/pass/eta_exp.h>

#include "mim/plug/mem/mem.h"
#include "mim/plug/mem/pass/copy_prop.h"
#include "mim/plug/mem/pass/reshape.h"
#include "mim/plug/mem/pass/ssa.h"
#include "mim/plug/mem/phase/add_mem.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) {
    MIM_REPL(stages, mem::remem_repl, {
        if (auto remem = Axm::isa<mem::remem>(def)) return remem->arg();
        return {};
    });

    MIM_REPL(stages, mem::alloc2malloc_repl, {
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
    Stage::hook<mem::add_mem_phase,  mem::phase::AddMem  >(stages);
    Stage::hook<mem::ssa_pass,       mem::pass:: SSA     >(stages);
    Stage::hook<mem::copy_prop_pass, mem::pass:: CopyProp>(stages);
    Stage::hook<mem::reshape_pass,   mem::pass:: Reshape >(stages);
    // clang-format on
}

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"mem", MIM_VERSION, mem::register_normalizers, reg_stages}; }
