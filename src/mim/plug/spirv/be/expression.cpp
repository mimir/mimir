
#include "mim/plug/core/autogen.h"
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

    auto original_def = def;
    def               = strip(def);
    if (!def) return -1;

    // Check if the stripped def was already emitted (e.g., via emit_term()) in strip_rec)
    if (auto it = locals_.find(def); it != locals_.end()) return it->second;

    auto type    = strip(def->type());
    Word type_id = emit_type(type);

    Word id = next_id();

    // Cache the stripped def if it's different from the original
    // This ensures that when the same underlying value is accessed via different paths,
    // we don't emit it multiple times
    if (def != original_def) locals_[def] = id;
    module_.id_names[id] = def->unique_name();

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
            // OpConstantComposite: result type is implicit, constituents are operands
            module_.declarations.emplace_back(Op{OpKind::ConstantComposite, constituents, id, type_id});
        } else {
            // OpCompositeConstruct: result type ID first, then constituents
            constituents.insert(constituents.begin(), type_id);
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
            // OpCompositeConstruct: result type ID first, then constituents
            constituents.insert(constituents.begin(), type_id);
            bb.ops.emplace_back(Op{OpKind::CompositeConstruct, constituents, id, type_id});
        }

        return id;
    }

    if (auto lit = def->isa<Lit>()) {
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
            if (emit_type(src->type()) == type_id) return src_id;
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
