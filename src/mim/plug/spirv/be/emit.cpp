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

    // Pre-pass: register every if/switch/loop constructor by its scope token, so
    // that exits can recover their target lams regardless of traversal order over
    // `muts`. For `if`/`switch` the scope token is op(0) of the argument tuple;
    // for the `loop` dispatch op(0) is the `Loop` capability, so we key by its
    // scope token. We also record the loop header lam (the lam holding the
    // dispatch) so loopbacks can branch back to it.
    //
    // The constructors are curried, so reconstruct an ordered argument tuple from
    // the curried args and store it under the scope token. Exits index into that
    // tuple by position (see `cf_args`).
    for (auto mut : muts) {
        auto lam = mut->isa<Lam>();
        if (!lam || !lam->is_set()) continue;
        auto app = lam->body()->isa<App>();
        if (!app) continue;
        if (Axm::isa<sflow::_if>(app)) {
            auto [token, cf_break, tuple, index, _arg] = app->uncurry_args<5>();
            cf_constructs_[token]                      = world().tuple({token, cf_break, tuple, index});
        } else if (Axm::isa<sflow::_switch>(app)) {
            auto [token, cf_break, cf_default, cases, index, _arg] = app->uncurry_args<6>();
            cf_constructs_[token] = world().tuple({token, cf_break, cf_default, cases, index});
        } else if (Axm::isa<sflow::loop>(app)) {
            auto [cf_struct, cf_break, cf_continue, cf_body, cond, _arg] = app->uncurry_args<6>();
            auto token              = scope_token_of(cf_struct);
            cf_constructs_[token]   = world().tuple({cf_struct, cf_break, cf_continue, cf_body, cond});
            cf_loop_headers_[token] = lam;
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
