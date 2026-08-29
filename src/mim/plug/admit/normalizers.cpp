#include <mim/check.h>
#include <mim/world.h>

#include "mim/plug/admit/admit.h" // IWYU pragma: keep

namespace mim::plug::admit {

template<unify> const Def* normalize_unify(const Def*, const Def*, const Def* arg) {
    auto t = arg->type()->zonk();
    if (!t->is_closed()) return nullptr;

    return arg;
}

template<extract> const Def* normalize_extract(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    auto tup    = callee->as<App>()->arg();
    auto t      = tup->type()->zonk();
    if (!t->is_closed()) return nullptr;

    return world.extract(tup, arg);
}

MIM_admit_NORMALIZER_IMPL

} // namespace mim::plug::admit
