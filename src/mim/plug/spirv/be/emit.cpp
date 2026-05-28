#include "mim/plug/spirv/be/emit.h"

#include "mim/plug/sflow/sflow.h" // IWYU pragma: keep
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

    Scheduler new_scheduler(nest);
    swap(scheduler_, new_scheduler);

    auto f = nest.root()->mut()->as<Lam>();
    emit_function(f);

    // Pre-pass: register loop headers by loopback-path so loopback can find them
    // regardless of traversal order over `muts`. The loopback path differs from
    // the header struct's path: it comes from the Header field in the continue
    // lam's signature.
    for (auto mut : muts) {
        auto lam = mut->isa<Lam>();
        if (!lam || !lam->is_set()) continue;
        auto app = lam->body()->isa<App>();
        if (!app) continue;
        if (auto cf_loop = Axm::isa<sflow::loop>(app)) {
            auto [sigma, arg]                              = cf_loop->uncurry_args<2>();
            auto [token, cf_break, cf_continue, cf_header] = sigma->projs<4>();
            auto continue_dom = cf_continue->type()->as<Pi>()->dom();
            auto header_field = continue_dom->op(3);
            auto path         = Axm::as<sflow::Header>(header_field)->uncurry_args<3>()[1];
            loop_headers_[path] = cf_header->as_mut<Lam>();
        }
    }

    for (auto mut : muts) {
        if (auto lam = mut->isa<Lam>()) {
            curr_function_ = lam;
            assert(lam == root() || Lam::isa_basicblock(lam));
            emit_bb(lam, lam2bb_[lam]);
        }
    }

    finalize_function(f);

    locals_.clear();

    assert_unused(lam2bb_.size() == old_size && "really make sure we didn't trigger a rehash");
}

} // namespace mim::plug::spirv
