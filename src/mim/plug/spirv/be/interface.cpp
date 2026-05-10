#include "mim/plug/spirv/autogen.h"
#include "mim/plug/spirv/be/emit.h"
#include "mim/plug/spirv/spirv.h" // IWYU pragma: keep

namespace mim::plug::spirv {

const Def* isa_inferface(const Def* def) {
    if (auto ptr = Axm::isa<spirv::Ptr>(def)) {
        auto [storage_class, n, decorations, wrapped_type] = ptr->uncurry_args<4>();
        auto [local, write]
            = Axm::as<spirv::StorageClass>(Axm::as<spirv::storage>(storage_class)->type())->uncurry_args<2>();
        if (!Lit::as<bool>(local)) return def;
    }
    // TODO
    return nullptr;
}

Word Emitter::emit_interface(std::string name, const Def* def) {
    if (interface_vars_.contains(def)) return interface_vars_[def];

    auto ptr                                           = Axm::as<spirv::Ptr>(def);
    auto [storage_class, n, decorations, wrapped_type] = ptr->uncurry_args<4>();
    auto _storage_class                                = Axm::as<spirv::storage>(storage_class);
    Word __storage_class                               = storage_class::from_mim(_storage_class.id());

    // Check if pointer type already exists
    Word ptr_type_id = emit_type(def);

    // store global interface variable in map
    // for access later
    Word interface_id    = next_id();
    interface_vars_[def] = interface_id;

    // emit var
    module_.declarations.emplace_back(Op{OpKind::Variable, {__storage_class}, interface_id, ptr_type_id});
    module_.id_names[interface_id] = std::format("{}", name);

    if (decorations->isa<Sigma>())
        for (auto decoration : decorations->ops())
            emit_decoration(interface_id, decoration);
    else
        emit_decoration(interface_id, decorations);

    return interface_id;
}

} // namespace mim::plug::spirv
