#include "mim/tuple.h"
#include "mim/world.h"

#include "mim/plug/torch/torch.h"

namespace mim::plug::torch {

const Def* normalize_resolve(const Def* type, const Def* callee, const Def* arg) {
    auto& world            = type->world();
    auto [key, instances]  = arg->projs<2>();
    auto instance_count    = Lit::isa(instances->arity());
    if (!instance_count) return nullptr;

    const Def* result = nullptr;
    for (size_t i = 0; i != *instance_count; ++i) {
        auto [candidate, value] = instances->proj(*instance_count, i)->projs<2>();
        if (candidate != key) continue;
        if (result) {
            mim::error(callee->loc(), "duplicate exact Torch interface instance for key '{}'", key);
            return world.bot(type);
        }
        result = value;
    }

    if (result) return result;
    if (key->isa<Var>()) return nullptr;
    mim::error(callee->loc(), "no exact Torch interface instance for key '{}'", key);
    return world.bot(type);
}

MIM_torch_NORMALIZER_IMPL

} // namespace mim::plug::torch
