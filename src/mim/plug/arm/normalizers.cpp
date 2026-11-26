#include <mim/world.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

namespace {

/// Fuses `sel.ne (cmp (cc.<c> f, 0), x, y)` into `sel.<c> (f, x, y)`.
template<auto id>
const Def* fuse_cc(const Def* type, const Def* arg) {
    using Sel = decltype(id);
    if constexpr (id == Sel::ne) {
        auto [f, x, y] = arg->projs<3>();
        if (auto c = Axm::isa<cmp>(f)) {
            auto [a, b] = c->args<2>();
            if (auto l = Lit::isa(b); l && *l == 0)
                if (auto cond = Axm::isa<cc>(a))
                    return type->world().call(cc_to<Sel>(cond.id()), Defs{cond->arg(), x, y});
        }
    }
    return nullptr;
}

} // namespace

template<csel id>
const Def* normalize_csel(const Def* type, const Def*, const Def* arg) {
    return fuse_cc<id>(type, arg);
}

template<fcsel id>
const Def* normalize_fcsel(const Def* type, const Def*, const Def* arg) {
    return fuse_cc<id>(type, arg);
}

MIM_arm_NORMALIZER_IMPL

} // namespace mim::plug::arm
