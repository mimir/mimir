#include "mim/plug/spirv/be/emit.h"

#include "mim/plug/spirv/spirv.h" // IWYU pragma: keep

namespace mim::plug::spirv {

Emitter::Emitter(World& world)
    : Super(world, "spirv_emitter", true) {}

void Emitter::visit(const Nest& nest) {
    auto muts = Scheduler::schedule(nest); // TODO make sure to not compute twice

    // make sure that we don't need to rehash later on
    for (auto mut : muts)
        if (auto lam = mut->isa<Lam>()) lam2bb_.try_emplace(lam, BB());
    auto old_size = lam2bb_.size();

    assert(root()->ret_var());

    auto f = nest.root()->mut()->as<Lam>();
    emit_function(f);

    Scheduler new_scheduler(nest);
    swap(scheduler_, new_scheduler);

    for (auto mut : muts) {
        if (auto lam = mut->isa<Lam>()) {
            curr_lam_ = lam;
            assert(lam == root() || Lam::isa_basicblock(lam));
            emit_bb(lam, lam2bb_[lam]);
        }
    }

    finalize_function(f);

    locals_.clear();

    assert_unused(lam2bb_.size() == old_size && "really make sure we didn't trigger a rehash");
}

} // namespace mim::plug::spirv
