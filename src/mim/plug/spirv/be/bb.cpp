#include "mim/lam.h"

#include "mim/plug/scf/scf.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"

namespace mim::plug::spirv {

/// The scope key identifying the control-flow construct that owns this
/// capability: the `(path, step)` pair. `id`/`gen` (the fork-uniqueness
/// witness/thread, see %scf.Gen/%scf.Id) are erased-machinery arguments the
/// backend doesn't need for scoping -- path/step alone already uniquely
/// identify a construct site within one activation, exactly as before their
/// introduction.
const Def* scope_key_of(const Def* cf_struct) {
    auto type = cf_struct->type();
    if (Axm::isa<scf::If>(type)) {
        auto [path, step, id, b] = Axm::as<scf::If>(type)->uncurry_args<4>();
        return cf_struct->world().tuple({path, step});
    }
    if (Axm::isa<scf::Switch>(type)) {
        auto [path, step, id, t, b] = Axm::as<scf::Switch>(type)->uncurry_args<5>();
        return cf_struct->world().tuple({path, step});
    }
    if (Axm::isa<scf::Loop>(type)) {
        auto [path, step, id, gen, b, h] = Axm::as<scf::Loop>(type)->uncurry_args<6>();
        return cf_struct->world().tuple({path, step});
    }
    error("not an scf control-flow capability: {}", cf_struct);
}

/// The same scope key, derived from a token value's type
/// (`Token path step gen`).
const Def* scope_key_of_token(const Def* token) {
    auto [path, step, gen] = Axm::as<scf::Token>(token->type())->uncurry_args<3>();
    return token->world().tuple({path, step});
}

/// Recovers the argument tuple `(token, ...targets...)` of the `if`/`switch`/`loop`
/// constructor that owns `cf_struct`, by looking it up via its scope key.
/// The map is populated by a pre-pass in `visit`.
/// This is how the actual target lams (continue/break/merge/header/cases) are
/// found now that the capability types no longer embed them.
const Def* Emitter::cf_args(const Def* cf_struct) {
    auto key = scope_key_of(cf_struct);
    auto it  = cf_constructs_.find(key);
    if (it == cf_constructs_.end()) error("no scf constructor registered for scope {}", key);
    return it->second;
}

Lam* Emitter::cf_latch(const Def* cf_struct) {
    auto key = scope_key_of(cf_struct);
    auto it  = cf_latches_.find(key);
    if (it == cf_latches_.end()) error("no latch registered for loop scope {}", key);
    return it->second;
}

void Emitter::link_phi(Lam* from, Lam* callee, const Def* arg) {
    DLOG("ordinary jump: {} -> {}", from, callee);
    if (strip(callee->var()->type()) == world().sigma()) return;
    link_phi(from, callee, emit_term(arg));
}

void Emitter::link_phi(Lam* from, Lam* callee, Word value_id) {
    auto phi = callee->var();
    if (strip(phi->type()) == world().sigma()) return;
    bb(callee).phis[phi].emplace_back(value_id);
    bb(callee).phis[phi].emplace_back(bb_id(from));
    locals_[phi] = emit_term(phi);
}

Word Emitter::emit_bb(Lam* lam, BB& bb) {
    // std::cerr << "hello from lam " << lam << "!\n";
    auto app = lam->body()->as<App>();

    Word lam_id = bb_id(lam);
    if (lam == root())
        set_id_name(lam_id, std::format("entry_{}", lam->unique_name()));
    else
        set_id_name(lam_id, lam->unique_name());

    bb.label = Op{OpKind::Label, {}, lam_id, {}};

    // emit bb end instruction
    if (app->callee() == ret_var_) {
        // plain CPS return lam called (fun-style; structured functions return
        // through the %scf.return branch below instead)
        // => OpReturn | OpReturnValue

        // always materialize arg first so side-effects (stores, etc.) emit
        auto arg = emit_term(app->arg());
        if (strip(app->arg()->type()) == world().sigma())
            bb.end = Op{OpKind::Return, {}, {}, {}};
        else
            bb.end = Op{OpKind::ReturnValue, {arg}, {}, {}};
    } else if (auto callee = Lam::isa_mut_basicblock(app->callee())) {
        // === Ordinary jump ===
        // => OpBranch

        link_phi(lam, callee, app->arg());
        bb.end = Op{OpKind::Branch, {bb_id(callee)}, {}, {}};
    } else if (auto cf_if = Axm::isa<scf::_if>(app)) {
        // === Structured if-else ===
        // => OpSelectionMerge + OpBranchConditional
        auto [token, cf_break, tuple, index, arg] = cf_if->uncurry_args<5>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {bb_id(cf_break->as_mut<Lam>()), 0},
            {},
            {}
        };
        std::vector<Word> branches{emit_term(index)};
        for (auto branch : tuple->ops()) {
            link_phi(lam, branch->as_mut<Lam>(), arg);
            branches.push_back(bb_id(branch->as_mut<Lam>()));
        }
        bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
    } else if (auto cf_switch = Axm::isa<scf::_switch>(app)) {
        // === Structured switch-case ===
        // => OpSelectionMerge + OpSwitch
        auto [token, cf_break, cf_default, cases, index, arg] = cf_switch->uncurry_args<6>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {bb_id(cf_break->as_mut<Lam>()), 0},
            {},
            {}
        };

        link_phi(lam, cf_default->as_mut<Lam>(), arg);
        std::vector<Word> case_ops{emit_term(index), bb_id(cf_default->as_mut<Lam>())};

        // cases is an array of dependent tuples `(index, continuation)`.
        for (auto c : cases->ops()) {
            auto [idx_def, case_def] = c->projs<2>();
            auto case_lam            = case_def->as_mut<Lam>();
            link_phi(lam, case_lam, arg);
            auto literal = int_to_words(Lit::as(idx_def), 32);
            case_ops.insert(case_ops.end(), literal.begin(), literal.end());
            case_ops.push_back(bb_id(case_lam));
        }
        bb.end = Op{OpKind::Switch, case_ops, {}, {}};
    } else if (auto cf_anchor = Axm::isa<scf::header>(app)) {
        // === Structured loop pre-header / anchor ===
        // The lam ending in `header` is just the predecessor of the SPIR-V loop
        // header. OpLoopMerge belongs in the header lam itself (see `loop`
        // case below), so all we do here is unconditionally branch into it.
        auto [token, cf_header, arg] = cf_anchor->uncurry_args<3>();
        auto header_lam              = cf_header->as_mut<Lam>();

        link_phi(lam, header_lam, arg);
        bb.end = Op{OpKind::Branch, {bb_id(header_lam)}, {}, {}};
    } else if (auto cf_loop = Axm::isa<scf::loop>(app)) {
        // === Loop header dispatch ===
        // => OpLoopMerge + OpBranch
        // This lam is the SPIR-V loop header. Emit OpLoopMerge naming the break
        // lam as merge block and the synthesized latch as continue target, then
        // unconditionally branch into the body. There is no direct edge to
        // break here: a `while`-style loop is written by placing a conditional
        // `%scf.break(l)` as the first thing in the body.
        auto [cf_struct, cf_break, cf_body, arg] = cf_loop->uncurry_args<4>();
        auto break_lam                           = cf_break->as_mut<Lam>();
        auto body_lam                            = cf_body->as_mut<Lam>();

        bb.merge = Op{
            OpKind::LoopMerge,
            {bb_id(break_lam), bb_id(cf_latch(cf_struct)), 0},
            {},
            {}
        };
        link_phi(lam, body_lam, arg);
        bb.end = Op{OpKind::Branch, {bb_id(body_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<scf::_continue>(app)) {
        // === Back-edge ===
        // Branch to the loop's synthesized latch block, which carries the
        // unique back-edge to the header (SPIR-V permits exactly one back-edge
        // block per loop).
        auto [inner_token, cf_struct, value] = cf_exit->uncurry_args<3>();
        auto latch                           = cf_latch(cf_struct);
        link_phi(lam, latch, value);
        bb.end = Op{OpKind::Branch, {bb_id(latch)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<scf::fallthrough>(app)) {
        // Fall through to the next case in execution order. The case array is in
        // reverse order, so the next case is the previous array entry; the last
        // one falls through to the switch's default (switch constructor arg 2).
        auto [inner_token, cf_struct, value] = cf_exit->uncurry_args<3>();
        auto switch_args                     = cf_args(cf_struct);
        auto cases                           = switch_args->op(3);
        auto target_lam                      = switch_args->op(2)->as_mut<Lam>(); // default
        for (size_t i = 0, e = cases->num_ops(); i != e; ++i) {
            if (cases->op(i)->op(1)->as_mut<Lam>() == lam) {
                if (i > 0) target_lam = cases->op(i - 1)->op(1)->as_mut<Lam>();
                break;
            }
        }
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa(scf::_break::s, app)) {
        // Break out of the switch (switch constructor arg 1).
        auto [inner_token, cf_struct, value] = cf_exit->uncurry_args<3>();
        auto target_lam                      = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa(scf::_break::l, app)) {
        // Break out of the loop (loop constructor arg 1).
        auto [inner_token, cf_struct, value] = cf_exit->uncurry_args<3>();
        auto target_lam                      = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<scf::merge>(app)) {
        // Merge the if (if constructor arg 1, the merge target).
        auto [merge_token, cf_struct, value] = cf_exit->uncurry_args<3>();
        auto target_lam                      = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_ret = Axm::isa<scf::_return>(app)) {
        // === Structured function return ===
        // `%scf.return token ret payload`, the only elimination of the opaque
        // %scf.Ret wrapper.
        // => OpReturn | OpReturnValue
        auto [token, ret, value] = cf_ret->uncurry_args<3>();

        // always materialize value first so side-effects (stores, etc.) emit
        auto value_id = emit_term(value);
        if (strip(value->type()) == world().sigma())
            bb.end = Op{OpKind::Return, {}, {}, {}};
        else
            bb.end = Op{OpKind::ReturnValue, {value_id}, {}, {}};
    } else if (auto cf_call = Axm::isa<scf::call>(app)) {
        // === Function call ===
        // The lam ends with `call token fn (t_val, ret_lam)`, where ret_lam
        // is the CPS continuation that receives the call's result. Lower to
        // OpFunctionCall followed by OpBranch into ret_lam, phi-ing the
        // returned value in.
        auto [token, fn, t_val, ret_lam_def] = cf_call->uncurry_args<4>();
        auto ret_lam                         = ret_lam_def->as_mut<Lam>();

        // The callee is a `%scf.Fn` (polymorphic return, no CPS ret_pi);
        // recover the result type from the caller-side continuation instead.
        auto ret_type       = strip(ret_lam->type()->as<Pi>()->dom());
        Word result_id      = next_id();
        Word result_type_id = emit_type(ret_type);

        auto fn_it = function_id(fn->as_mut<Lam>());
        std::vector<Word> operands{fn_it, emit_term(t_val)};
        bb.tail.emplace_back(Op{OpKind::FunctionCall, operands, result_id, result_type_id});

        link_phi(lam, ret_lam, result_id);
        bb.end = Op{OpKind::Branch, {bb_id(ret_lam)}, {}, {}};
    } else if (app->callee()->isa<Bot>()) {
        // unreachable
        // => OpUnreachable
        bb.end = Op{OpKind::Unreachable, {}, {}, {}};
    } else {
        error("unsupported control flow terminator in lam '{}' ({}): callee '{}' ({}) is not a known scf primitive",
              lam, lam->gid(), app->callee(), app->callee()->gid());
    }

    return lam_id;
}

} // namespace mim::plug::spirv
