#include "mim/phase.h"

#include "mim/plug/scf/scf.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"
#include "mim/plug/spirv/spirv.h"

namespace mim::plug::spirv {

namespace {

/// Collects all `spirv::variable` defs reachable from a function root.
class InterfaceCollector : public Analysis {
public:
    InterfaceCollector(World& world)
        : Analysis(world, "spirv_interface_collector") {}

    DefVec collect(Lam* root) {
        rewrite(root);
        return std::move(vars_);
    }

    const Def* rewrite_imm_App(const App* app) override {
        if (auto var = Axm::isa<spirv::variable>(app)) {
            auto [storage_class, decs, type] = var->uncurry_args<3>();
            auto sc                          = storage_class::from_mim(Axm::as<spirv::storage>(storage_class).id());
            if (!storage_class::is_local(sc)) vars_.push_back(app);
        }
        return Rewriter::rewrite_imm_App(app);
    }

private:
    DefVec vars_;
};

} // namespace

void Emitter::emit_decoration(Word var_id, const Def* decoration_) {
    if (auto builtin = Axm::isa<spirv::builtin>(decoration_)) {
        auto magic = spv_builtin::from_mim(builtin.id());
        module_.annotations.emplace_back(Op{
            OpKind::Decorate,
            {var_id, decoration::BuiltIn, static_cast<Word>(magic)},
            {},
            {},
        });
        set_id_name(var_id, std::format("{}_{}", spv_builtin::name(magic), var_id));
        return;
    }
    auto decoration = Axm::as<spirv::decor>(decoration_);
    switch (decoration.id()) {
        case spirv::decor::location:
            auto location = decoration->arg();
            module_.annotations.emplace_back(Op{
                OpKind::Decorate,
                {var_id, decoration::Location, static_cast<Word>(Lit::as(location))},
                {},
                {},
            });
            break;
    }
}

Word Emitter::emit_function(Lam* function) {
    curr_function_ = function;

    Word id = function_id(function);

    // Convert Pi type to direct style and strip
    const Pi* type      = strip(function->type())->as<Pi>();
    Word type_id        = emit_type(type);
    Word return_type_id = emit_type(type->codom());
    module_.funDefinitions.emplace_back(Op{
        OpKind::Function,
        {0, type_id},
        id,
        return_type_id
    });
    set_id_name(id, function->unique_name());

    // Handle function parameter
    auto var      = root()->var();
    auto var_type = type->dom();
    Word var_id   = next_id();
    if (var_type != world().sigma())
        module_.funDefinitions.emplace_back(Op{OpKind::FunctionParameter, {}, var_id, emit_type(var_type)});
    set_id_name(var_id, var->unique_name());
    locals_[world().extract(var, (size_t)0)] = var_id;
    // Also map the whole var: con-style functions project params directly off
    // it, so the mem-materialization path in strip_rec reaches the raw var via
    // `emit_term(extract->tuple())` — without this entry that would fall into
    // emit_term_into and emit the var's unstripped (token/ret) type.
    locals_[var] = var_id;

    // external lams are emitted as entry points
    if (root()->is_external()) {
        // Handle entry point markers and interface variables
        std::optional<spirv::model> model{};
        DefVec exec_modes{};

        // fun-style CPS lams nest their params: dom = [[params...], Cn R];
        // con-style structured functions (polymorphic `%scf.Ret` param)
        // list their params directly.
        auto sigma = root()->ret_pi() ? root()->dom()->op(0)->as<Sigma>() : root()->dom()->as<Sigma>();
        for (size_t idx = 0; idx < sigma->num_ops(); ++idx) {
            auto param = sigma->op(idx);

            // Check if this is an entry point marker
            if (auto entry_marker = Axm::isa<spirv::entry>(param)) {
                if (model.has_value()) error("multiple execution model markers found in entry point");

                // Extract model and modes: entry: Model → {n: Nat} → «n; Mode» → ★
                auto [model_, modes] = entry_marker->uncurry_args<2>();
                model                = Axm::as<spirv::model>(model_).id();

                // Collect execution modes
                if (auto mode = Axm::isa<spirv::mode>(modes))
                    exec_modes.push_back(mode);
                else
                    for (auto mode : modes->ops())
                        exec_modes.push_back(mode);
                continue;
            }
        }

        // assume compute shader if no builtins were used
        if (!model.has_value()) error(root()->loc(), "external lam without entry param");

        Word model_magic;
        switch (*model) {
            case spirv::model::vertex: model_magic = 0; break;
            case spirv::model::fragment: model_magic = 4; break;
            case spirv::model::compute: model_magic = 5; break;
        }

        Op entry{
            OpKind::EntryPoint,
            {model_magic, id},
            {},
            {}
        };

        // append name
        for (Word word : string_to_words(root()->sym().str()))
            entry.operands.push_back(word);

        InterfaceCollector collector{world()};
        for (auto var : collector.collect(function))
            entry.operands.push_back(emit_term(var));

        module_.entryPoints.push_back(entry);

        // Emit execution modes
        for (auto mode_def : exec_modes) {
            if (auto mode = Axm::isa<spirv::mode>(mode_def)) {
                Word mode_magic;
                switch (mode.id()) {
                    case spirv::mode::invocations: mode_magic = execution_mode::Invocations; break;
                    case spirv::mode::spacing_equal: mode_magic = execution_mode::SpacingEqual; break;
                    case spirv::mode::spacing_fractional_even:
                        mode_magic = execution_mode::SpacingFractionalEven;
                        break;
                    case spirv::mode::spacing_fractional_odd: mode_magic = execution_mode::SpacingFractionalOdd; break;
                    case spirv::mode::vertex_order_cw: mode_magic = execution_mode::VertexOrderCw; break;
                    case spirv::mode::vertex_order_ccw: mode_magic = execution_mode::VertexOrderCcw; break;
                    case spirv::mode::pixel_center_integer: mode_magic = execution_mode::PixelCenterInteger; break;
                    case spirv::mode::origin_upper_left: mode_magic = execution_mode::OriginUpperLeft; break;
                    case spirv::mode::origin_lower_left: mode_magic = execution_mode::OriginLowerLeft; break;
                    case spirv::mode::early_fragment_tests: mode_magic = execution_mode::EarlyFragmentTests; break;
                    default: continue;
                }
                module_.executionModes.push_back(Op{
                    OpKind::ExecutionMode,
                    {id, mode_magic},
                    {},
                    {}
                });
            }
        }
    }

    return id;
}

void Emitter::append_bb(BB& bb) {
    // reserve space for ops
    module_.funDefinitions.reserve(1 + bb.merge.has_value() + bb.phis.size() + bb.ops.size() + bb.tail.size() + 1);

    module_.funDefinitions.push_back(bb.label);
    for (auto [phi, var_parents] : bb.phis)
        module_.funDefinitions.emplace_back(Op{OpKind::Phi, var_parents, emit_term(phi), emit_type(phi->type())});
    module_.funDefinitions.insert(module_.funDefinitions.end(), bb.ops.begin(), bb.ops.end());
    module_.funDefinitions.insert(module_.funDefinitions.end(), bb.tail.begin(), bb.tail.end());
    if (bb.merge) module_.funDefinitions.push_back(*bb.merge);
    module_.funDefinitions.push_back(bb.end);
}

void Emitter::layout_append(Lam* lam, MutSet& done) {
    if (!done.emplace(lam).second) return;
    assert(lam2bb_.contains(lam));
    append_bb(lam2bb_[lam]);

    auto app = lam->body()->isa<App>();
    if (!app) return;

    // Peel curried applications to find a polymorphic return at the bottom of
    // the app chain (see emit_bb).
    auto callee_base = app->callee();
    while (auto curried = callee_base->isa<App>()) callee_base = curried->callee();
    if (callee_base == ret_var_) return;

    if (auto callee = Lam::isa_mut_basicblock(app->callee())) {
        layout_append(callee, done);
    } else if (auto cf_if = Axm::isa<scf::_if>(app)) {
        auto [token, cf_break, tuple, index, arg] = cf_if->uncurry_args<5>();
        for (auto branch : tuple->ops()) layout_append(branch->as_mut<Lam>(), done);
        layout_append(cf_break->as_mut<Lam>(), done);
    } else if (auto cf_switch = Axm::isa<scf::_switch>(app)) {
        auto [token, cf_break, cf_default, cases, index, arg] = cf_switch->uncurry_args<6>();
        // The case array is in reverse execution order; walk the fall-through
        // chain, which ends in the default, then the break target.
        for (size_t i = cases->num_ops(); i-- != 0;) layout_append(cases->op(i)->op(1)->as_mut<Lam>(), done);
        layout_append(cf_default->as_mut<Lam>(), done);
        layout_append(cf_break->as_mut<Lam>(), done);
    } else if (auto cf_anchor = Axm::isa<scf::header>(app)) {
        auto [token, cf_header, arg] = cf_anchor->uncurry_args<3>();
        layout_append(cf_header->as_mut<Lam>(), done);
    } else if (auto cf_loop = Axm::isa<scf::loop>(app)) {
        auto [cf_struct, cf_break, cf_body, arg] = cf_loop->uncurry_args<4>();
        layout_append(cf_body->as_mut<Lam>(), done);
        layout_append(cf_latch(cf_struct), done);
        layout_append(cf_break->as_mut<Lam>(), done);
    } else if (auto cf_call = Axm::isa<scf::call>(app)) {
        auto [token, fn, t_val, ret_lam] = cf_call->uncurry_args<4>();
        layout_append(ret_lam->as_mut<Lam>(), done);
    }
    // continue/fallthrough/break/merge edges are not followed: their targets
    // (latch, sibling case, break/merge lam) belong to the construct that
    // introduced them and are laid out by that construct's case above.
}

void Emitter::finalize_function(Lam* fun) {
    // Block layout is derived from the scf terminators alone, no CFG: each
    // construct site lays out its regions first and its exit (break/merge
    // target, latch for loops) behind them. Exit targets are exactly the
    // multi-predecessor blocks, every other block has a unique predecessor
    // (its dominator) and is placed right in the walk — so blocks appear
    // after their dominators and construct blocks stay contiguous.
    MutSet done;
    layout_append(fun, done);

    // Blocks the structural walk did not reach (unreachable code) still own
    // referenced ids; append them behind everything reachable.
    for (auto mut : Scheduler::schedule(nest())) {
        if (auto lam = mut->isa_mut<Lam>(); lam && !done.contains(lam)) {
            append_bb(lam2bb_[lam]);
            if (auto it = latch_of_header_.find(lam); it != latch_of_header_.end() && !done.contains(it->second))
                append_bb(lam2bb_[it->second]);
        }
    }

    module_.funDefinitions.emplace_back(Op{OpKind::FunctionEnd, {}, {}, {}});
}

} // namespace mim::plug::spirv
