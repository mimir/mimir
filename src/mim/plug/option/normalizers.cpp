#include <mim/world.h>

#include "mim/plug/option/option.h"

namespace mim::plug::option {

const Def* normalize_unwrap_unsafe(const Def*, const Def*, const Def* arg) {
    if (auto name = arg->isa<Name>()) arg = name->op();
    if (auto inj = arg->isa<Inj>(); inj && inj->index() == Opt_ctor::Some) return inj->value();

    return nullptr;
}

MIM_option_NORMALIZER_IMPL

} // namespace mim::plug::option
