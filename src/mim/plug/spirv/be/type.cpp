#include "mim/def.h"

#include "mim/plug/math/math.h"
#include "mim/plug/mem/mem.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"
#include "mim/plug/spirv/spirv.h" // IWYU pragma: keep

namespace mim::plug::spirv {

Word Emitter::emit_type(const Def* type) {
    // check if already converted
    if (auto i = types_.find(type); i != types_.end()) return i->second;

    // create new id
    Word id              = next_id();
    module_.id_names[id] = "ERROR";

    if (type == world().sigma()) {
        module_.declarations.emplace_back(Op{OpKind::TypeVoid, {}, id, {}});
        module_.id_names[id] = "void";
    } else if (Idx::isa(type)) {
        // All index types map to a single shared 32-bit unsigned integer type
        if (i32_type_id_.has_value()) {
            types_[type] = *i32_type_id_;
            return *i32_type_id_;
        }
        module_.declarations.emplace_back(Op{
            OpKind::TypeInt,
            {32, 0},
            id,
            {}
        });
        module_.id_names[id] = "u32";
        i32_type_id_         = id;
    } else if (type->isa<Nat>() || Idx::isa(type)) {
        module_.declarations.emplace_back(Op{
            OpKind::TypeInt,
            {32, 1},
            id,
            {}
        });
        module_.id_names[id] = "i32";
        i32_type_id_         = id;
    } else if (auto w = math::isa_f(type)) {
        Word bitwidth = static_cast<Word>(*w);
        module_.declarations.emplace_back(Op{
            OpKind::TypeFloat,
            {bitwidth, 0},
            id,
            {}
        });
        module_.id_names[id] = std::format("f{}", bitwidth);
    } else if (auto b = Axm::isa<spirv::Bool>(type)) {
        module_.declarations.emplace_back(Op{OpKind::TypeBool, {}, id, {}});
        module_.id_names[id] = "bool";
    } else if (auto arr = type->isa<Arr>()) {
        // Convert the element type
        Word elem_id = emit_type(arr->body());

        if (auto arity_lit = Lit::isa(arr->arity())) {
            u64 size = *arity_lit;

            // use vector for small arrays of scalars (2, 3, or 4 elements)
            // TODO: sizes 8 and 16 are also supported for some memory
            // models, add check
            if (size >= 2 && size <= 4 && is_scalar_type(arr->body())) {
                module_.declarations.emplace_back(Op{
                    OpKind::TypeVector,
                    {elem_id, static_cast<Word>(size)},
                    id,
                    {}
                });
                module_.id_names[id] = std::format("vec{}_{}", size, id_name(elem_id));
            } else {
                // always use i32 for arity
                Word length_id = emit_term(world().lit_idx(1ull << 32, size));

                module_.declarations.emplace_back(Op{
                    OpKind::TypeArray,
                    {elem_id, length_id},
                    id,
                    {}
                });
                module_.id_names[id] = std::format("arr{}_{}", size, id_name(elem_id));
            }
        } else {
            // TODO: runtime-sized arrays
            std::cerr << "dynamic arrays not yet supported\n";
            fe::unreachable();
        }
    } else if (auto pi = type->isa<Pi>()) {
        Word param_type  = emit_type(pi->dom());
        Word return_type = emit_type(pi->codom());

        // Do not emit void argument type
        std::vector<Word> ops{return_type};
        if (pi->dom() != world().sigma()) ops.push_back(param_type);

        Op op{OpKind::TypeFunction, ops, id, {}};
        module_.declarations.push_back(op);
        module_.id_names[id] = std::format("pi{}", type->unique_name());
    } else if (auto sigma = type->isa<Sigma>()) {
        if (sigma->isa_mut()) std::cerr << "mutable sigmas not yet supported\n";

        std::vector<Word> fields{};
        for (auto t : sigma->ops())
            fields.emplace_back(emit_type(t));

        module_.declarations.emplace_back(Op{OpKind::TypeStruct, fields, id, {}});
        module_.id_names[id] = std::format("sigma{}", type->unique_name());
    } else if (auto ptr = Axm::isa<spirv::Ptr>(type)) {
        auto [storage_class_, pointee] = ptr->uncurry_args<2>();
        auto storage_class             = storage_class::from_mim(Axm::as<spirv::storage>(storage_class_).id());
        Op op{
            OpKind::TypePointer,
            {storage_class, emit_type(pointee)},
            id
        };
        module_.declarations.push_back(op);
        module_.id_names[id]
            = std::format("ptr_{}_{}", storage_class::name(storage_class), id_name(emit_type(pointee)));
    }

    if (module_.id_names[id] == "ERROR") std::cerr << "unsupported type: " << type << "\n";

    types_[type] = id;
    return id;
}

} // namespace mim::plug::spirv
