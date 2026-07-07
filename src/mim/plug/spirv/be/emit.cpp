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

    // Pre-pass: register every if/switch/loop constructor by its scope key
    // -- a `(path, step)` tuple, see `scope_key_of` -- so that exits can
    // recover their target lams regardless of traversal order over `muts`.
    // For `if`/`switch` the key is derived from the token operand (op(0) of
    // the argument tuple); for the `loop` dispatch op(0) is the `Loop`
    // capability, so we key by its scope key directly.
    //
    // For every loop we also synthesize a latch lam that just jumps to the
    // header. All `%sflow.continue` sites branch to the latch, so it carries
    // the loop's only back-edge — SPIR-V permits exactly one back-edge block
    // per loop, and funneling every continue through the latch guarantees
    // that by construction. The latch doubles as the OpLoopMerge continue
    // target.
    //
    // The constructors are curried, so reconstruct an ordered argument tuple from
    // the curried args and store it under the scope key. Exits index into that
    // tuple by position (see `cf_args`).
    Vector<Lam*> latches;
    for (auto mut : muts) {
        auto lam = mut->isa<Lam>();
        if (!lam || !lam->is_set()) continue;
        auto app = lam->body()->isa<App>();
        if (!app) continue;
        if (Axm::isa<sflow::_if>(app)) {
            auto [token, cf_break, tuple, index, _arg] = app->uncurry_args<5>();
            cf_constructs_[scope_key_of_token(token)]  = world().tuple({token, cf_break, tuple, index});
        } else if (Axm::isa<sflow::_switch>(app)) {
            auto [token, cf_break, cf_default, cases, index, _arg] = app->uncurry_args<6>();
            cf_constructs_[scope_key_of_token(token)] = world().tuple({token, cf_break, cf_default, cases, index});
        } else if (Axm::isa<sflow::loop>(app)) {
            auto [cf_struct, cf_break, cf_body, _arg] = app->uncurry_args<4>();
            auto key            = scope_key_of(cf_struct);
            cf_constructs_[key] = world().tuple({cf_struct, cf_break, cf_body});

            auto latch = world().mut_con(lam->dom())->set("latch");
            latch->app(false, lam, latch->var());
            // The latch is not part of the nest, so its var never goes through
            // the scheduler; pre-allocate its phi id instead.
            locals_[strip(latch->var())] = next_id();
            lam2bb_.try_emplace(latch, BB());
            cf_latches_[key]      = latch;
            latch_of_header_[lam] = latch;
            latches.push_back(latch);
        }
    }
    auto old_size = lam2bb_.size();

    // Identify the return continuation: a plain CPS ret var (fun-style) or a
    // parameter of polymorphic sflow return type (`%sflow.Ret`, con-style).
    ret_var_ = root()->ret_var();
    if (!ret_var_)
        for (size_t i = 0, e = root()->num_vars(); i != e; ++i)
            if (isa_ret(root()->var(i)->type())) {
                ret_var_ = root()->var(i);
                break;
            }
    assert(ret_var_ && "function has neither a CPS ret var nor an %sflow.Ret parameter");

    Scheduler new_scheduler(nest);
    swap(scheduler_, new_scheduler);

    auto f = nest.root()->mut()->as<Lam>();
    emit_function(f);

    for (auto mut : muts) {
        if (auto lam = mut->isa<Lam>()) {
            curr_function_ = lam;
            assert(lam == root() || Lam::isa_basicblock(lam));
            emit_bb(lam, lam2bb_[lam]);
        }
    }
    for (auto latch : latches) {
        curr_function_  = latch;
        auto& latch_bb  = lam2bb_[latch];
        emit_bb(latch, latch_bb);

        // A loop that never calls `%sflow.continue` leaves its latch unreached:
        // no site ever links a value into `latch->var()`, so it gets no OpPhi.
        // The header's own OpPhi still lists the latch as a source (the latch
        // unconditionally branches back to the header), so synthesize an
        // OpUndef for the id instead of leaving it referenced but never defined.
        auto var = latch->var();
        if (strip(var->type()) != world().sigma() && !latch_bb.phis.contains(var))
            latch_bb.ops.emplace_back(Op{OpKind::Undefined, {}, locals_[strip(var)], emit_type(var->type())});
    }

    finalize_function(f);

    locals_.clear();

    assert_unused(lam2bb_.size() == old_size && "really make sure we didn't trigger a rehash");
}

} // namespace mim::plug::spirv
