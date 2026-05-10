#include "mim/lam.h"

#include "mim/plug/sflow/sflow.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"

namespace mim::plug::spirv {

void Emitter::link_phi(Lam* lam, Lam* callee, const Def* arg) {
    DLOG("ordinary jump: {} -> {}", lam, callee);
    auto phi = callee->var();
    bb(callee).phis[phi].emplace_back(emit_term(arg));
    bb(callee).phis[phi].emplace_back(id(lam));
    locals_[phi] = id(phi);
    bb(lam).end  = Op{OpKind::Branch, {id(callee)}, {}, {}};
}

Word Emitter::emit_bb(Lam* lam, BB& bb) {
    std::cerr << "hello from lam " << lam << "!\n";
    auto app = lam->body()->as<App>();

    Word lam_id              = next_id();
    locals_[lam]             = lam_id;
    module_.id_names[lam_id] = lam->unique_name();

    bb.label = Op{OpKind::Label, {}, lam_id, {}};

    // emit bb end instruction
    if (app->callee() == root()->ret_var()) {
        // return lam called
        // => OpReturn | OpReturnValue

        std::vector<Word> values;
        std::vector<const Def*> types;

        for (auto arg : app->args()) {
            auto value    = emit_term(arg);
            auto arg_type = strip(arg->type());
            if (arg_type != world().sigma()) {
                values.emplace_back(value);
                types.emplace_back(arg_type);
            }
        }

        switch (values.size()) {
            case 0: bb.end = Op{OpKind::Return, {}, {}, {}}; break;
            case 1: bb.end = Op{OpKind::ReturnValue, {values[0]}, {}, {}}; break;
            default:
                Word val_id = next_id();
                Word type   = emit_type(world().sigma(types));
                bb.tail.emplace_back(Op{OpKind::CompositeConstruct, {values}, val_id, type});
        }

    } else if (auto callee = Lam::isa_mut_basicblock(app->callee())) {
        // === Ordinary jump ===
        // => OpBranch

        link_phi(lam, callee, app->arg());
        bb.end = Op{OpKind::Branch, {id(callee)}, {}, {}};
    } else if (auto cf_if = Axm::isa<sflow::_if>(app->callee())) {
        // === Structured if-else ===
        // => OpSelectionMerge + OpBranchConditional
        auto [token, cf_break, tuple, index] = cf_if->uncurry_args<4>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {id(cf_break), 0},
            {},
            {}
        };
        std::vector<Word> branches{emit_term(index)};
        for (auto branch : tuple->ops()) {
            link_phi(lam, branch->as_mut<Lam>(), app->arg());
            branches.push_back(id(branch));
        }
        bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
    } else if (auto cf_switch = Axm::isa<sflow::_switch>(app->callee())) {
        // === Structured switch-case ===
        // => OpSelectionMerge + OpSwitch
        auto [token, cf_break, cf_default, targets, index] = cf_switch->uncurry_args<5>();

        bb.merge = Op{
            OpKind::SelectionMerge,
            {id(cf_break), 0},
            {},
            {}
        };

        link_phi(lam, cf_default->as_mut<Lam>(), app->arg());
        std::vector<Word> cases{emit_term(index), id(cf_default)};

        // targets is a right-nested tuple: [idx0, case0, [idx1, case1, [..., []]]].
        // Walk the spine, peeling one case per level.
        for (auto cur = targets; cur->num_ops() == 3; cur = cur->op(2)) {
            auto idx_def  = cur->op(0);
            auto case_lam = cur->op(1);
            link_phi(lam, case_lam->as_mut<Lam>(), app->arg());
            auto literal = int_to_words(Lit::as(idx_def), 32);
            cases.insert(cases.end(), literal.begin(), literal.end());
            cases.push_back(id(case_lam));
        }
        bb.end = Op{OpKind::Switch, cases, {}, {}};
    } else if (auto cf_loop = Axm::isa<sflow::loop>(app->callee())) {
        // === Structured loop pre-header ===
        // The lam ending in `loop` is just the predecessor of the SPIR-V loop
        // header. OpLoopMerge belongs in the header lam itself (see `header`
        // case below), so all we do here is unconditionally branch into it.
        auto [token, cf_break, cf_continue, cf_header] = cf_loop->uncurry_args<4>();
        auto header_lam                                = cf_header->as_mut<Lam>();

        link_phi(lam, header_lam, app->arg());
        bb.end = Op{OpKind::Branch, {id(header_lam)}, {}, {}};
    } else if (auto cf_header = Axm::isa<sflow::header>(app->callee())) {
        // === Loop header ===
        // => OpLoopMerge + OpBranch/OpBranchConditional
        // This lam is the SPIR-V loop header. Emit OpLoopMerge naming the break
        // lam as merge block and the continue lam as continue target. Then
        // branch into the body via the tuple/index pair (mirrors `if`).
        // Register `lam` so loopbacks reaching this loop can find it.
        auto [token, cf_struct, tuple, index] = cf_header->uncurry_args<4>();
        auto [path, cf_continue, cf_break]    = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        auto continue_lam                     = cf_continue->as_mut<Lam>();
        auto break_lam                        = cf_break->as_mut<Lam>();

        loop_headers_[path] = lam;

        bb.merge = Op{
            OpKind::LoopMerge,
            {id(break_lam), id(continue_lam), 0},
            {},
            {}
        };
        std::vector<Word> branches{emit_term(index)};
        for (auto branch : tuple->ops()) {
            link_phi(lam, branch->as_mut<Lam>(), app->arg());
            branches.push_back(id(branch));
        }
        bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
    } else if (auto cf_exit = Axm::isa<sflow::_continue>(app->callee())) {
        auto cf_struct                             = cf_exit->arg();
        auto [path, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        link_phi(lam, continue_target->as_mut<Lam>(), app->arg());
    } else if (auto cf_exit = Axm::isa<sflow::fallthrough>(app->callee())) {
        auto cf_struct                             = cf_exit->arg();
        auto [path, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        link_phi(lam, continue_target->as_mut<Lam>(), app->arg());
    } else if (auto cf_exit = Axm::isa<sflow::_break>(app->callee())) {
        auto cf_struct                             = cf_exit->arg();
        auto [path, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        link_phi(lam, break_target->as_mut<Lam>(), app->arg());
    } else if (auto cf_exit = Axm::isa<sflow::loopback>(app->callee())) {
        // === Loopback to header ===
        // The arg has type `Header H path break`; `path` is the unique key of
        // the enclosing loop, registered by the header lam in `loop_headers_`.
        auto cf_header_val      = cf_exit->arg();
        auto [_H, path, _break] = Axm::as<sflow::Header>(cf_header_val->type())->uncurry_args<3>();
        auto it                 = loop_headers_.find(path);
        if (it == loop_headers_.end()) error("loopback target not registered: {}", lam);
        auto header_lam = it->second;

        link_phi(lam, header_lam, app->arg());
        bb.end = Op{OpKind::Branch, {id(header_lam)}, {}, {}};
    } else if (auto cf_branch = Axm::isa<sflow::branch>(app->callee())) {
        // === Unconditional forward branch ===
        // => OpBranch
        auto [token, callee] = cf_branch->uncurry_args<2>();
        auto callee_lam      = callee->as_mut<Lam>();
        link_phi(lam, callee_lam, app->arg());
        bb.end = Op{OpKind::Branch, {id(callee_lam)}, {}, {}};
    } else if (auto cf_call = Axm::isa<sflow::call>(app->callee())) {
        // === Function call ===
        // The lam ends with `call(token, fn)(t_val, ret_lam)`, where ret_lam
        // is the CPS continuation that receives the call's result. Lower to
        // OpFunctionCall followed by OpBranch into ret_lam, phi-ing the
        // returned value in.
        auto [token, fn] = cf_call->uncurry_args<2>();
        auto t_val       = app->arg(0);
        auto ret_lam     = app->arg(1)->as_mut<Lam>();

        auto ret_type       = strip(fn->type()->as<Pi>()->ret_pi()->dom());
        Word result_id      = next_id();
        Word result_type_id = emit_type(ret_type);

        std::vector<Word> operands{id(fn), emit_term(t_val)};
        bb.tail.emplace_back(Op{OpKind::FunctionCall, operands, result_id, result_type_id});

        auto phi     = ret_lam->var();
        auto& ret_bb = this->bb(ret_lam);
        ret_bb.phis[phi].emplace_back(result_id);
        ret_bb.phis[phi].emplace_back(id(lam));
        locals_[phi] = id(phi);
        bb.end       = Op{OpKind::Branch, {id(ret_lam)}, {}, {}};
    } else if (app->callee()->isa<Bot>()) {
        // unreachable
        // => OpUnreachable
        bb.end = Op{OpKind::Unreachable, {}, {}, {}};
    }

    return lam_id;
}

} // namespace mim::plug::spirv
