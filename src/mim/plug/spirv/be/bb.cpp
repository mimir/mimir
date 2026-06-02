#include "mim/lam.h"

#include "mim/plug/sflow/sflow.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"

namespace mim::plug::spirv {

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
    if (Lam::isa_returning(lam))
        module_.id_names[lam_id] = std::format("entry_{}", lam->unique_name());
    else
        module_.id_names[lam_id] = lam->unique_name();

    bb.label = Op{OpKind::Label, {}, lam_id, {}};

    // emit bb end instruction
    if (app->callee() == root()->ret_var()) {
        // return lam called
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
    } else if (auto cf_if = Axm::isa<sflow::_if>(app)) {
        // === Structured if-else ===
        // => OpSelectionMerge + OpBranchConditional
        auto [sigma, arg]                       = cf_if->uncurry_args<2>();
        auto [token, cf_break, tuple, index]    = sigma->projs<4>();

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
    } else if (auto cf_switch = Axm::isa<sflow::_switch>(app->callee())) {
        // === Structured switch-case ===
        // => OpSelectionMerge + OpSwitch
        auto [sigma, arg]                                       = app->uncurry_args<2>();
        auto [token, cf_break, cf_default, targets, index]      = sigma->projs<5>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {bb_id(cf_break->as_mut<Lam>()), 0},
            {},
            {}
        };

        link_phi(lam, cf_default->as_mut<Lam>(), app->arg());
        std::vector<Word> cases{emit_term(index), bb_id(cf_default->as_mut<Lam>())};

        // targets is a right-nested tuple: [idx0, case0, [idx1, case1, [..., []]]].
        // Walk the spine, peeling one case per level.
        for (auto cur = targets; cur->num_ops() == 3; cur = cur->op(2)) {
            auto idx_def  = cur->op(0);
            auto case_lam = cur->op(1);
            link_phi(lam, case_lam->as_mut<Lam>(), arg);
            auto literal = int_to_words(Lit::as(idx_def), 32);
            cases.insert(cases.end(), literal.begin(), literal.end());
            cases.push_back(bb_id(case_lam->as_mut<Lam>()));
        }
        bb.end = Op{OpKind::Switch, cases, {}, {}};
    } else if (auto cf_loop = Axm::isa<sflow::loop>(app)) {
        // === Structured loop pre-header ===
        // The lam ending in `loop` is just the predecessor of the SPIR-V loop
        // header. OpLoopMerge belongs in the header lam itself (see `header`
        // case below), so all we do here is unconditionally branch into it.
        auto [sigma, arg]                                   = cf_loop->uncurry_args<2>();
        auto [token, cf_break, cf_continue, cf_header]      = sigma->projs<4>();
        auto header_lam                                     = cf_header->as_mut<Lam>();

        link_phi(lam, header_lam, arg);
        bb.end = Op{OpKind::Branch, {bb_id(header_lam)}, {}, {}};
    } else if (auto cf_header = Axm::isa<sflow::header>(app)) {
        // === Loop header ===
        // => OpLoopMerge + OpBranch/OpBranchConditional
        // This lam is the SPIR-V loop header. Emit OpLoopMerge naming the break
        // lam as merge block and the continue lam as continue target. Then
        // branch into the body via the tuple/index pair (mirrors `if`).
        // Register `lam` so loopbacks reaching this loop can find it.
        auto [sigma, arg]                          = cf_header->uncurry_args<2>();
        auto [token, cf_struct, tuple, index]      = sigma->projs<4>();
        auto [_tok, cf_continue, cf_break]    = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto continue_lam                     = cf_continue->as_mut<Lam>();
        auto break_lam                        = cf_break->as_mut<Lam>();

        bb.merge = Op{
            OpKind::LoopMerge,
            {bb_id(break_lam), bb_id(continue_lam), 0},
            {},
            {}
        };
        std::vector<Word> branches{emit_term(index)};
        for (auto branch : tuple->ops()) {
            link_phi(lam, branch->as_mut<Lam>(), arg);
            branches.push_back(bb_id(branch->as_mut<Lam>()));
        }
        bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::_continue>(app)) {
        auto [sigma, value]                         = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct]               = sigma->projs<2>();
        auto [token, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto target_lam                             = continue_target->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::fallthrough>(app)) {
        auto [sigma, value]                         = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct]               = sigma->projs<2>();
        auto [token, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto target_lam                             = continue_target->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::_break>(app)) {
        auto [sigma, value]                         = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct]               = sigma->projs<2>();
        auto [token, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto target_lam                             = break_target->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::merge>(app)) {
        auto [sigma, value]                         = cf_exit->uncurry_args<2>();
        auto [merge_token, cf_struct]               = sigma->projs<2>();
        auto [token, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto target_lam                             = break_target->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::loopback>(app)) {
        // === Loopback to header ===
        // The arg has type `Header H path break`; `path` is the unique key of
        // the enclosing loop, registered by the header lam in `loop_headers_`.
        auto [sigma, arg]                = cf_exit->uncurry_args<2>();
        auto [cf_header_val, token]      = sigma->projs<2>();
        auto [_H, path, _break]     = Axm::as<sflow::Header>(cf_header_val->type())->uncurry_args<3>();
        auto it                     = loop_headers_.find(path);
        if (it == loop_headers_.end()) error("loopback target not registered: {}", lam);
        auto header_lam = it->second;

        link_phi(lam, header_lam, arg);
        bb.end = Op{OpKind::Branch, {bb_id(header_lam)}, {}, {}};
    } else if (auto cf_branch = Axm::isa<sflow::branch>(app)) {
        // === Unconditional forward branch ===
        // => OpBranch
        auto [callee, token, value] = cf_branch->uncurry_args<3>();
        auto callee_lam             = callee->as_mut<Lam>();
        link_phi(lam, callee_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(callee_lam)}, {}, {}};
    } else if (auto cf_call = Axm::isa<sflow::call>(app)) {
        // === Function call ===
        // The lam ends with `call(token, fn)(t_val, ret_lam)`, where ret_lam
        // is the CPS continuation that receives the call's result. Lower to
        // OpFunctionCall followed by OpBranch into ret_lam, phi-ing the
        // returned value in.
        auto [fn, token, t_val, ret_lam_def] = cf_call->uncurry_args<4>();
        auto ret_lam                         = ret_lam_def->as_mut<Lam>();

        auto ret_type       = fn->type()->as<Pi>()->ret_pi()->dom();
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
        error("unsupported control flow terminator in lam '{}' ({}): callee '{}' ({}) is not a known sflow primitive",
              lam, lam->gid(), app->callee(), app->callee()->gid());
    }

    return lam_id;
}

} // namespace mim::plug::spirv
