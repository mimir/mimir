#include "mim/axm.h"
#include "mim/world.h"

#include "mim/plug/spirv/spirv.h" // IWYU pragma: keep

namespace mim::plug::spirv {

template<spirv::storage id>
const Def* normalize_loadable(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();

    if (auto storage_class = Axm::isa<spirv::storage>(arg)) {
        switch (id) {
            case spirv::storage::INPUT: return world.lit_bool(true);
            case spirv::storage::OUTPUT: return world.lit_bool(false);
            case spirv::storage::UNIFORM: return world.lit_bool(true);
            default: return world.lit_bool(false);
        }
    }
}

template<spirv::storage id>
const Def* normalize_storable(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();

    if (auto storage_class = Axm::isa<spirv::storage>(arg)) {
        switch (id) {
            case spirv::storage::INPUT: return world.lit_bool(false);
            case spirv::storage::OUTPUT: return world.lit_bool(true);
            case spirv::storage::UNIFORM: return world.lit_bool(false);
            default: return world.lit_bool(false);
        }
    }
}

} // namespace mim::plug::spirv
