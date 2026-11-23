#include <mim/world.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

template<csel id>
const Def* normalize_csel(const Def* type, const Def*, const Def* arg) {
    if constexpr (id == csel::ne) {
        auto [f, x, y] = arg->projs<3>();
        if (auto c = Axm::isa<cmp>(f)) {
            auto [a, b] = c->args<2>();
            if (auto l = Lit::isa(b); l && *l == 0)
                if (auto cond = Axm::isa<cc>(a))
                    return type->world().call(cc_to_csel(cond.id()), Defs{cond->arg(), x, y});
        }
    }
    return nullptr;
}

MIM_arm_NORMALIZER_IMPL

} // namespace mim::plug::arm
