#include <mim/world.h>

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem {

const Def* normalize_lea(const Def*, const Def*, const Def* arg) {
    auto [ptr, index]          = arg->projs<2>();
    auto [pointee, addr_space] = Axm::as<Ptr>(ptr->type())->args<2>();

    if (auto a = Lit::isa(pointee->arity()); a && *a == 1) return ptr;

    return {};
}

const Def* normalize_load(const Def* type, const Def*, const Def* arg) {
    auto& world                = type->world();
    auto [mem, ptr]            = arg->projs<2>();
    auto [pointee, addr_space] = Axm::as<Ptr>(ptr->type())->args<2>();

    if (ptr->isa<Bot>()) return world.tuple({mem, world.bot(type->as<Sigma>()->op(1))});

    // loading an empty tuple can only result in an empty tuple
    // TODO use common singleton magic here
    if (auto sigma = pointee->isa<Sigma>(); sigma && sigma->num_ops() == 0)
        return world.tuple({mem, world.tuple(sigma->type(), {})});

    return {};
}

const Def* normalize_remem(const Def*, const Def*, const Def*) { return {}; }

const Def* normalize_store(const Def*, const Def*, const Def* arg) {
    auto [mem, ptr, val] = arg->projs<3>();

    if (ptr->isa<Bot>() || val->isa<Bot>()) return mem;
    if (auto pack = val->isa<Pack>(); pack && pack->body()->isa<Bot>()) return mem;
    if (auto tuple = val->isa<Tuple>()) {
        if (std::ranges::all_of(tuple->ops(), [](const Def* op) { return op->isa<Bot>(); })) return mem;
    }

    return {};
}

MIM_mem_NORMALIZER_IMPL

} // namespace mim::plug::mem
