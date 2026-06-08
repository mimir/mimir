
#include "mim/plug/core/core.h"
#include "mim/plug/math/math.h"
#include "mim/plug/spirv/autogen.h"
#include "mim/plug/spirv/be/emit.h"
#include "mim/plug/vec/autogen.h"

namespace mim::plug::spirv {

// Builds a private pointer type with private storage class and given pointee type.
const Def* ptr_type_private(const Def* type) {
    auto& world = type->world();
    return world.call<spirv::Ptr>(world.annex<spirv::storage>(spirv::storage::PRIVATE), type);
}

Word Emitter::emit_term_into(const Def* def, BB& bb) {
    std::cerr << "emit_bb: " << def->unique_name() << " (node: " << def->node_name() << "): " << def->type()
              << std::endl;

    auto type    = def->type();
    Word type_id = emit_type(type);

    Word id = next_id();

    // Cache emitted result so subsequent emit_term calls reuse the same id.
    locals_[def] = id;
    set_id_name(id, def->unique_name());

    if (auto tuple = def->isa<Tuple>()) {
        std::vector<Word> constituents;

        // Unit value
        if (tuple == world().tuple()) return id;

        // Emit all tuple elements
        for (size_t i = 0, n = tuple->num_projs(); i != n; ++i) {
            auto elem = tuple->proj(n, i);

            constituents.push_back(emit_term(elem));
        }

        // Directly unpack tuple if it only has a single or no values
        if (constituents.empty()) return emit_term(world().tuple());
        if (constituents.size() == 1) return constituents[0];

        if (is_const(tuple)) {
            // OpConstantComposite: result type implicit, constituents are operands
            module_.declarations.emplace_back(Op{OpKind::ConstantComposite, constituents, id, type_id});
        } else {
            // OpCompositeConstruct: serializer prints result_type already
            bb.ops.emplace_back(Op{OpKind::CompositeConstruct, constituents, id, type_id});
        }

        return id;
    }

    if (auto pack = def->isa<Pack>()) {
        auto arity = pack->arity();
        auto body  = pack->body();

        auto arity_val = 0;
        if (auto lit = Lit::isa(arity))
            arity_val = *lit;
        else
            error("arrays of dynamic size are not supported yet");

        Word body_id = emit_term(body);
        std::vector<Word> constituents;
        for (int i = 0; i < arity_val; i++)
            constituents.push_back(body_id);

        if (is_const(pack)) {
            // OpConstantComposite: result type is implicit, constituents are operands
            module_.declarations.emplace_back(Op{OpKind::ConstantComposite, constituents, id, type_id});
        } else {
            // OpCompositeConstruct: serializer prints result_type already
            bb.ops.emplace_back(Op{OpKind::CompositeConstruct, constituents, id, type_id});
        }

        return id;
    }

    if (auto lit = def->isa<Lit>()) {
        // Literals are hash-consed, so `unique_name()` may hand back a borrowed annex
        // symbol from some `let` sharing the value (e.g. `0:Nat` aliases
        // `%core.mode.us`). Override with the gid-based name to avoid the misleading
        // (though sanitized) symbol.
        set_id_name(id, std::format("_{}", def->gid()));

        if (lit->type()->isa<Nat>()) {
            // Nat: assume 32-bit for now
            // hints = {type=0 (int), width}
            module_.declarations.push_back(Op{
                OpKind::Constant,
                int_to_words(lit->get(), 32),
                id,
                emit_type(lit->type()),
                {0, 32}
            });
            globals_[def] = id;
            return id;
        }

        if (Idx::isa(lit->type())) {
            module_.declarations.push_back(Op{
                OpKind::Constant,
                int_to_words(lit->get(), 32),
                id,
                emit_type(lit->type()),
                {0, 32}
            });
            globals_[def] = id;
            return id;
        }

        if (auto w = math::isa_f(lit->type())) {
            // Float: hints = {type=1 (float), width}
            int width = static_cast<int>(*w);
            module_.declarations.push_back(Op{
                OpKind::Constant,
                float_to_words(lit->get(), width),
                id,
                emit_type(lit->type()),
                {1, width}
            });
            globals_[def] = id;
            return id;
        }
    }

    if (auto cat = Axm::isa<vec::cat>(def)) {
        auto [nm, T, vs] = cat->uncurry_args<3>();
        auto [n, m]      = nm->projs<2>();
        auto [as, bs]    = vs->projs<2>();
        std::vector<Word> constituents;

        for (auto [n, vs] : {
                 std::pair{n, as},
                 std::pair{m, bs}
        }) {
            if (auto size = Lit::isa(n)) {
                if (size > 1) {
                    auto array = vs->type()->as<Arr>();
                    for (Word index = 0; index < size; index++) {
                        Word id      = next_id();
                        Word body_id = emit_type(array->body());
                        constituents.push_back(id);
                        bb.ops.push_back(Op{
                            OpKind::CompositeExtract,
                            {emit_term(vs), index},
                            id,
                            body_id,
                        });
                    }
                } else {
                    // size == 1, as size == 0 would be normalized away
                    constituents.push_back(emit_term(vs));
                }
            } else {
                error("runtime sized arrays not supported yet");
            }
        }

        bb.ops.push_back(Op{
            OpKind::CompositeConstruct,
            {constituents},
            id,
            type_id,
        });
        return id;
    }

    if (auto extract = def->isa<Extract>()) {
        auto tuple = extract->tuple();
        auto index = extract->index();

        std::cerr << "  extract from tuple: " << tuple->unique_name() << " (node: " << tuple->node_name() << ")"
                  << std::endl;
        std::cerr << "  index: " << index << std::endl;
        std::cerr << "  tuple is Var: " << (tuple->isa<Var>() ? "yes" : "no") << std::endl;
        if (tuple->isa<Var>())
            std::cerr << "  tuple in locals_: " << (locals_.count(tuple) ? "yes" : "no") << std::endl;

        // for literal indices, use OpCompositeExtract
        if (auto lit = Lit::isa(index)) {
            Word index = static_cast<Word>(*lit);

            bb.ops.push_back(Op{
                OpKind::CompositeExtract,
                {emit_term(tuple), index},
                id,
                type_id
            });

            return id;
        }

        // Dynamic indices require OpAccessChain
        // We need to:
        // 1. Create a pointer type for the composite
        // 2. Create a variable in Function storage with the composite as initializer
        // 3. Use OpAccessChain to get a pointer to the element
        // 4. Use OpLoad to load the value
        if (is_const(tuple)) {
            Word composite_id = emit_term(tuple);

            // Get or create pointer type for the composite
            Word ptr_type_id = emit_type(ptr_type_private(tuple->type()));

            // Create variable with initializer
            Word var_id = next_id();
            module_.declarations.push_back(Op{
                OpKind::Variable,
                {storage_class::Private, composite_id},
                var_id,
                ptr_type_id
            });

            // Get or create pointer type for the element
            Word elem_ptr_type_id = emit_type(ptr_type_private(type));

            // OpAccessChain to get pointer to element
            Word ptr_id = next_id();
            bb.ops.push_back(Op{
                OpKind::AccessChain,
                {var_id, emit_term(index)},
                ptr_id,
                elem_ptr_type_id
            });

            // OpLoad to load the value
            bb.ops.push_back(Op{OpKind::Load, {ptr_id}, id, type_id});

            return id;
        }
    }

    if (auto var = Axm::isa<spirv::variable>(def)) {
        auto [storage_class, decs, type] = var->uncurry_args<3>();
        auto storage_class_              = storage_class::from_mim(Axm::as<spirv::storage>(storage_class).id());
        auto place                       = module_.declarations;
        if (storage_class_ == storage_class::Function) {
            // Place function var in function
            place = function_vars_;
        } else {
            // Add all other vars to globals
            globals_[def] = id;
        }
        place.push_back(Op{OpKind::Variable, {}, id, storage_class_});

        if (decs->isa<Sigma>() || decs->isa<Tuple>())
            for (auto dec : decs->ops())
                emit_decoration(id, dec);
        else
            emit_decoration(id, decs);
        return id;
    }

    if (auto store = Axm::isa<spirv::store>(def)) {
        auto [mem, global, value] = store->arg()->projs<3>();
        emit_term(mem);
        bb.ops.emplace_back(Op{
            OpKind::Store,
            {emit_term(global), emit_term(value)},
            {},
            {}
        });
        return id;
    }

    if (auto load = Axm::isa<spirv::load>(def)) {
        auto [mem, global] = load->arg()->projs<2>();
        emit_term(mem);
        bb.ops.emplace_back(Op{
            OpKind::Load,
            {emit_term(global)},
            id,
            type_id,
        });
        return id;
    }

    if (auto bitcast = Axm::isa<core::bitcast>(def)) {
        auto src    = bitcast->arg();
        auto src_id = emit_term(src);

        auto size2width = [&](const Def* type) -> nat_t {
            if (type->isa<Nat>()) return 32;
            if (Idx::isa(type)) return 32;
            return 0;
        };

        auto src_width = size2width(src->type());
        auto dst_width = size2width(bitcast->type());

        if (src_width == dst_width) {
            // Same size: use OpBitcast or just return source
            if (emit_type(src->type()) == type_id) {
                locals_[def] = src_id;
                return src_id;
            }
            bb.ops.push_back(Op{OpKind::Bitcast, {src_id}, id, type_id});
        } else if (src_width < dst_width) {
            // Widening: use OpUConvert (zero extend)
            bb.ops.push_back(Op{OpKind::UConvert, {src_id}, id, type_id});
        } else {
            // Narrowing: use OpUConvert (truncate)
            bb.ops.push_back(Op{OpKind::UConvert, {src_id}, id, type_id});
        }
        return id;
    }

    // Handle Nat arithmetic - Nat is emitted as 32-bit unsigned int
    if (auto nat = Axm::isa<core::nat>(def)) {
        auto [lhs, rhs] = nat->arg()->projs<2>();
        Word lhs_id     = emit_term(lhs);
        Word rhs_id     = emit_term(rhs);

        OpKind op_kind;
        switch (nat.id()) {
            case core::nat::add: op_kind = OpKind::IAdd; break;
            // TODO: %core.nat.sub saturates at 0; plain OpISub underflows on a<b.
            case core::nat::sub: op_kind = OpKind::ISub; break;
            case core::nat::mul: op_kind = OpKind::IMul; break;
            default:
                std::cerr << "unknown core.nat variant\n";
                bb.ops.emplace_back(Op{OpKind::Undefined, {}, id, type_id});
                return id;
        }

        bb.ops.emplace_back(Op{op_kind, {lhs_id, rhs_id}, id, type_id});
        return id;
    }

    // Handle Nat comparisons - Nat is emitted as 32-bit unsigned int
    if (auto ncmp = Axm::isa<core::ncmp>(def)) {
        auto [lhs, rhs]   = ncmp->arg()->projs<2>();
        Word lhs_id       = emit_term(lhs);
        Word rhs_id       = emit_term(rhs);
        Word bool_type_id = emit_type(world().annex<spirv::Bool>());

        OpKind op_kind;
        switch (ncmp.id()) {
            case core::ncmp::e:  op_kind = OpKind::IEqual; break;
            case core::ncmp::ne: op_kind = OpKind::INotEqual; break;
            case core::ncmp::l:  op_kind = OpKind::ULessThan; break;
            case core::ncmp::le: op_kind = OpKind::ULessThanEqual; break;
            case core::ncmp::g:  op_kind = OpKind::UGreaterThan; break;
            case core::ncmp::ge: op_kind = OpKind::UGreaterThanEqual; break;
            case core::ncmp::f:
                module_.declarations.emplace_back(Op{OpKind::ConstantFalse, {}, id, bool_type_id});
                return id;
            case core::ncmp::t:
                module_.declarations.emplace_back(Op{OpKind::ConstantTrue, {}, id, bool_type_id});
                return id;
            default:
                std::cerr << "unknown core.ncmp variant\n";
                bb.ops.emplace_back(Op{OpKind::Undefined, {}, id, bool_type_id});
                return id;
        }

        bb.ops.emplace_back(Op{op_kind, {lhs_id, rhs_id}, id, bool_type_id});
        return id;
    }

    // Handle math comparisons - SPIR-V comparisons always return OpTypeBool
    if (auto cmp = Axm::isa<math::cmp>(def)) {
        auto [lhs, rhs]   = cmp->arg()->projs<2>();
        Word lhs_id       = emit_term(lhs);
        Word rhs_id       = emit_term(rhs);
        Word bool_type_id = emit_type(world().annex<spirv::Bool>());

        OpKind op_kind;
        switch (cmp.id()) {
            // Ordered comparisons (false if either is NaN)
            case math::cmp::e: op_kind = OpKind::FOrdEqual; break;
            case math::cmp::ne: op_kind = OpKind::FOrdNotEqual; break;
            case math::cmp::l: op_kind = OpKind::FOrdLessThan; break;
            case math::cmp::le: op_kind = OpKind::FOrdLessThanEqual; break;
            case math::cmp::g: op_kind = OpKind::FOrdGreaterThan; break;
            case math::cmp::ge: op_kind = OpKind::FOrdGreaterThanEqual; break;
            // Unordered comparisons (true if either is NaN)
            case math::cmp::ue: op_kind = OpKind::FUnordEqual; break;
            case math::cmp::une: op_kind = OpKind::FUnordNotEqual; break;
            case math::cmp::ul: op_kind = OpKind::FUnordLessThan; break;
            case math::cmp::ule: op_kind = OpKind::FUnordLessThanEqual; break;
            case math::cmp::ug: op_kind = OpKind::FUnordGreaterThan; break;
            case math::cmp::uge: op_kind = OpKind::FUnordGreaterThanEqual; break;
            // Special cases
            case math::cmp::f: // always false
                module_.declarations.emplace_back(Op{OpKind::ConstantFalse, {}, id, bool_type_id});
                return id;
            case math::cmp::t: // always true
                module_.declarations.emplace_back(Op{OpKind::ConstantTrue, {}, id, bool_type_id});
                return id;
            case math::cmp::o: // ordered (neither is NaN) - use FOrdEqual(x,x) && FOrdEqual(y,y)
            case math::cmp::u: // unordered (either is NaN) - use FUnordNotEqual(x,x) || FUnordNotEqual(y,y)
                std::cerr << "math.cmp.o and math.cmp.u not yet implemented\n";
                bb.ops.emplace_back(Op{OpKind::Undefined, {}, id, bool_type_id});
                return id;
            default:
                std::cerr << "unknown math.cmp variant\n";
                bb.ops.emplace_back(Op{OpKind::Undefined, {}, id, bool_type_id});
                return id;
        }

        bb.ops.emplace_back(Op{
            op_kind,
            {lhs_id, rhs_id},
            id,
            bool_type_id
        });
        return id;
    }

    if (def->isa<Var>()) {
        // Var nodes should have been registered in locals_ already (via phi handling)
        if (auto it = locals_.find(def); it != locals_.end()) return it->second;
        std::cerr << "Var not in locals_: " << def->unique_name() << "\n";
    }

    bb.ops.emplace_back(Op{OpKind::Undefined, {}, id, type_id});
    if (auto app = def->isa<App>())
        std::cerr << "def not yet implemented: " << app->callee() << ": " << def->type() << "\n";
    else
        std::cerr << "def not yet implemented: " << def << " (node: " << def->node_name() << "): " << def->type()
                  << "\n";

    return id;
}

} // namespace mim::plug::spirv
