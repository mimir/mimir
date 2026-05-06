#include "mim/world.h"

#include "mim/plug/option/option.h"

namespace mim::plug::option {

const Def* normalize_unwrap_unsafe(const Def*, const Def*, const Def* arg) {
    if (auto inj = arg->isa<Inj>()) return inj->op(0);

    return nullptr;
}

MIM_option_NORMALIZER_IMPL

} // namespace mim::plug::option
