#include "mim/lam.h"

#include "mim/plug/sflow/autogen.h"
#include "mim/plug/spirv/be/emit.h"

#include "fe/assert.h"
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
        auto [header, cf_break, tuple, index, arg] = cf_if->uncurry_args<5>();

        bb.merge = Op{OpKind::SelectionMerge, {id(cf_break)}, {}, {}};
        std::vector<Word> branches{emit_term(index)};
        for (auto branch : tuple->ops()) {
            link_phi(lam, branch->as_mut<Lam>(), arg);
            branches.push_back(id(branch));
        }
        bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
    } else if (auto cf_switch = Axm::isa<sflow::_switch>(app->callee())) {
        // === Structured switch-case ===
        // => OpSelectionMerge + OpSwitch
        auto [header, cf_break, cf_default, targets, index, arg] = cf_switch->uncurry_args<6>();

        bb.merge = Op{OpKind::SelectionMerge, {id(cf_break)}, {}, {}};
        std::vector<Word> cases{emit_term(index)};
        for (auto cf_case : targets->ops()) {
            auto [index, body] = cf_case->ops<2>();
            link_phi(lam, body->as_mut<Lam>(), arg);
            auto literal = int_to_words(Lit::as(index), 32);
            cases.insert(cases.end(), literal.begin(), literal.end());
            cases.push_back(id(cf_case));
        }
        bb.end = Op{OpKind::BranchConditional, cases, {}, {}};
    } else if (auto cf_loop = Axm::isa<sflow::loop>(app->callee())) {
        // === Structured loop ===
        // => OpLoopMerge + OpBranch/OpBranchConditional
        auto [header, cf_break, tuple, index, arg] = cf_loop->uncurry_args<5>();

        bb.merge = Op{OpKind::LoopMerge, {id(cf_break)}, {}, {}};
        if (index == world().lit_tt()) {
            bb.end = Op{OpKind::Branch, {id(world().extract(tuple, 1))}, {}, {}};
        } else {
            std::vector<Word> branches{emit_term(index)};
            for (auto branch : tuple->ops()) {
                link_phi(lam, branch->as_mut<Lam>(), arg);
                branches.push_back(id(branch));
            }
            bb.end = Op{OpKind::BranchConditional, branches, {}, {}};
        }
    } else if (auto cf_exit = Axm::isa<sflow::exit>(app->callee())) {
        auto cf_struct                               = cf_exit->arg();
        auto [header, continue_target, break_target] = Axm::as<sflow::Struct>(cf_struct->type())->uncurry_args<3>();
        switch (cf_exit.id()) {
            case sflow::exit::_continue: link_phi(lam, continue_target->as_mut<Lam>(), app->arg()); break;
            case sflow::exit::_break: link_phi(lam, continue_target->as_mut<Lam>(), app->arg()); break;
            default: fe::unreachable();
        }

    } else if (auto dispatch = sflow::Dispatch(app)) {
        std::cerr << "dispatch handling for: " << app->unique_name() << std::endl;
        for (auto callee : dispatch.tuple()->projs([](const Def* def) { return def->isa_mut<Lam>(); })) {
            std::cerr << "  callee: " << callee->unique_name() << std::endl;
            auto arg = emit(dispatch.arg());
            auto phi = callee->var();

            // Don't emit phi's without actual values
            if (Axm::isa<mem::M>(phi->type())) break;
            if (phi->type() == world().sigma()) break;

            if (!lam2bb_.contains(callee->as_mut<Lam>())) std::cerr << "oh no! " << callee << "\n";
            lam2bb_[callee].phis[phi].emplace_back(arg);
            lam2bb_[callee].phis[phi].emplace_back(id(lam));
            locals_[phi] = id(phi);
        }

        // Set up phi for merge block
        auto merge_var     = dispatch.merge()->var();
        locals_[merge_var] = id(merge_var);

        // Emit structured control flow merge instructions if this is sflow.branch or sflow.loop
        if (dispatch.kind() == sflow::Dispatch::Kind::Branch) {
            bb.tail.emplace_back(Op{OpKind::SelectionMerge, {id(dispatch.merge())}, {}, {}});
        } else if (dispatch.kind() == sflow::Dispatch::Kind::Loop) {
            bb.tail.emplace_back(Op{
                OpKind::LoopMerge,
                {id(dispatch.merge()), id(dispatch.cont())},
                {},
                {}
            });
        }

        std::cerr << "dispatch.num_targets()=" << dispatch.num_targets() << std::endl;
        if (dispatch.num_targets() == 2) {
            // if cond { A args… } else { B args… }
            // => OpBranchConditional
            std::cerr << "BranchConditional: index=" << dispatch.index() << std::endl;
            std::cerr << "  tuple=" << dispatch.tuple() << std::endl;
            std::cerr << "  tuple deps:" << std::endl;
            for (auto dep : dispatch.tuple()->deps())
                std::cerr << "    " << dep << " (node: " << dep->node_name() << ")" << std::endl;
            Op op{OpKind::BranchConditional, {emit(dispatch.index())}, {}, {}};
            for (auto callee : dispatch.tuple()->ops())
                op.operands.push_back(id(callee));
            std::cerr << "  operands count: " << op.operands.size() << std::endl;
            bb.end = op;
        } else {
            // switch (index) { case 0: A args…; …; case n: Z args… }
            // => OpSwitch selector default_label [literal, label]*
            // Use last case as default since all cases are explicit in MimIR
            auto callees   = dispatch.tuple()->deps();
            auto num_cases = callees.size();
            assert(num_cases > 0);

            Op op{OpKind::Switch, {emit(dispatch.index())}, {}, {}};
            op.operands.push_back(id(callees[num_cases - 1])); // default = last case
            for (size_t i = 0; i + 1 < num_cases; ++i) {
                op.operands.push_back(int_to_words(i, 32)[0]);
                op.operands.push_back(id(callees[i]));
            }
            bb.end = op;
        }
    } else if (app->callee()->isa<Bot>()) {
        // unreachable
        // => OpUnreachable
        bb.end = Op{OpKind::Unreachable, {}, {}, {}};
    } else if (Pi::isa_returning(app->callee_type())) {
        // function call
        // => Op
    }
    // TODO: OpTerminateInvocation
}
} // namespace mim::plug::spirv
