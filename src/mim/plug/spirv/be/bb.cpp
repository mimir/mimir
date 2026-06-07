#include "mim/lam.h"

#include "mim/plug/sflow/sflow.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"

namespace mim::plug::spirv {

/// The scope token identifying the control-flow construct that owns this
/// capability.
/// It is the first explicit argument of the `If`/`Switch`/`Loop` type.
static const Def* scope_token_of(const Def* cf_struct) {
    auto type = cf_struct->type();
    if (Axm::isa<sflow::If>(type)) return Axm::as<sflow::If>(type)->uncurry_args<2>()[0];
    if (Axm::isa<sflow::Switch>(type)) return Axm::as<sflow::Switch>(type)->uncurry_args<3>()[0];
    if (Axm::isa<sflow::Loop>(type)) return Axm::as<sflow::Loop>(type)->uncurry_args<5>()[0];
    error("not an sflow control-flow capability: {}", cf_struct);
}

/// Recovers the argument tuple `(token, ...targets...)` of the `if`/`switch`/`loop`
/// constructor that owns `cf_struct`, by looking it up via its scope token.
/// The map is populated by a pre-pass in `visit`.
/// This is how the actual target lams (continue/break/merge/header/cases) are
/// found now that the capability types no longer embed them.
const Def* Emitter::cf_args(const Def* cf_struct) {
    auto token = scope_token_of(cf_struct);
    auto it    = cf_constructs_.find(token);
    if (it == cf_constructs_.end()) error("no sflow constructor registered for token {}", token);
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
        auto [sigma, arg]                                  = app->uncurry_args<2>();
        auto [token, cf_break, cf_default, cases, index]   = sigma->projs<5>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {bb_id(cf_break->as_mut<Lam>()), 0},
            {},
            {}
        };

        link_phi(lam, cf_default->as_mut<Lam>(), app->arg());
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
        auto [sigma, arg]                     = cf_header->uncurry_args<2>();
        auto [token, cf_struct, tuple, index] = sigma->projs<4>();
        // Recover break/continue targets from the enclosing loop constructor.
        auto loop_args    = cf_args(cf_struct);
        auto break_lam    = loop_args->op(1)->as_mut<Lam>();
        auto continue_lam = loop_args->op(2)->as_mut<Lam>();

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
        // Continue to the loop's continue block (loop constructor arg 2).
        auto [sigma, value]           = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct] = sigma->projs<2>();
        auto target_lam               = cf_args(cf_struct)->op(2)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::fallthrough>(app)) {
        // Fall through to the next case in execution order. The case array is in
        // reverse order, so the next case is the previous array entry; the last
        // one falls through to the switch's default (switch constructor arg 2).
        auto [sigma, value]           = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct] = sigma->projs<2>();
        auto switch_args              = cf_args(cf_struct);
        auto cases                    = switch_args->op(3);
        auto target_lam               = switch_args->op(2)->as_mut<Lam>(); // default
        for (size_t i = 0, e = cases->num_ops(); i != e; ++i) {
            if (cases->op(i)->op(1)->as_mut<Lam>() == lam) {
                if (i > 0) target_lam = cases->op(i - 1)->op(1)->as_mut<Lam>();
                break;
            }
        }
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa(sflow::_break::s, app)) {
        // Break out of the switch (switch constructor arg 1).
        auto [sigma, value]           = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct] = sigma->projs<2>();
        auto target_lam               = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa(sflow::_break::l, app)) {
        // Break out of the loop (loop constructor arg 1).
        auto [sigma, value]           = cf_exit->uncurry_args<2>();
        auto [inner_token, cf_struct] = sigma->projs<2>();
        auto target_lam               = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::merge>(app)) {
        // Merge the if (if constructor arg 1, the merge target).
        auto [sigma, value]           = cf_exit->uncurry_args<2>();
        auto [merge_token, cf_struct] = sigma->projs<2>();
        auto target_lam               = cf_args(cf_struct)->op(1)->as_mut<Lam>();
        link_phi(lam, target_lam, value);
        bb.end = Op{OpKind::Branch, {bb_id(target_lam)}, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::loopback>(app)) {
        // === Loopback to header ===
        // Branch back to the loop header (loop constructor arg 3).
        auto [sigma, arg]            = cf_exit->uncurry_args<2>();
        auto [loopback_token, cf_struct] = sigma->projs<2>();
        auto header_lam              = cf_args(cf_struct)->op(3)->as_mut<Lam>();
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
