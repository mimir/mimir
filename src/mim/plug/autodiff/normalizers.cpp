#include <mim/axm.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/autodiff/autodiff.h"

namespace mim::plug::autodiff {

/// Currently this normalizer does nothin.
/// TODO: Maybe we want to handle trivial lookup replacements here.
const Def* normalize_ad(const Def*, const Def*, const Def*) { return {}; }

const Def* normalize_AD(const Def*, const Def*, const Def* arg) {
    auto ad_ty = autodiff_type_fun(arg);
    if (ad_ty) return ad_ty;
    return {};
}

const Def* normalize_Tangent(const Def*, const Def*, const Def* arg) { return tangent_type_fun(arg); }

/// Currently this normalizer does nothing.
/// We usually want to keep zeros as long as possible to avoid unnecessary allocations.
/// A high-level addition with zero can be shortened directly.
const Def* normalize_zero(const Def*, const Def*, const Def*) { return {}; }

/// Currently resolved the full addition.
/// There is no benefit in keeping additions around longer than necessary.
const Def* normalize_add(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();

    // TODO: add tuple -> tuple of adds
    // TODO: add zero -> other
    // TODO: unify mapping over structure with other aspects like zero

    auto T      = callee->as<App>()->arg();
    auto [a, b] = arg->projs<2>();

    if (Axm::isa<zero>(a)) return b;
    if (Axm::isa<zero>(b)) return a;
    // A value level match would be harder as a tuple might in reality be a var or extract
    if (auto sig = T->isa<Sigma>()) {
        auto p   = sig->num_ops(); // TODO: or num_projs
        auto ops = DefVec(p, [&](size_t i) {
            return world.app(world.app(world.annex<add>(), sig->op(i)), {a->proj(i), b->proj(i)});
        });
        return world.tuple(ops);
    } else if (auto arr = T->isa<Arr>()) {
        // TODO: is this working for non-lit (non-tuple) or do we need a loop?
        auto pack      = world.mut_pack(T);
        auto body_type = arr->body();
        pack->set(world.app(world.app(world.annex<add>(), body_type),
                            {world.extract(a, pack->var()), world.extract(b, pack->var())}));
        return pack;
    } else if (Idx::isa(type)) {
        return world.call(core::wrap::add, 0_n, Defs{a, b});
    } else if (Axm::isa<mem::M>(type)) {
        // TODO: mem stays here (only resolved after direct simplification)
        return {};
    } else if (T->isa<App>()) {
        assert(0 && "not handled");
    }

    return {};
}

const Def* normalize_sum(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();

    auto [count, T] = callee->as<App>()->args<2>();

    if (auto lit = count->isa<Lit>()) {
        auto val  = lit->get<nat_t>();
        auto args = arg->projs(val);
        auto sum  = world.app(world.annex<zero>(), T);
        // This special case would also be handled by add zero
        if (val >= 1) sum = args[0];
        for (size_t i = 1; i < val; ++i)
            sum = world.app(world.app(world.annex<add>(), T), {sum, args[i]});
        return sum;
    }
    assert(0);
    return {};
}

MIM_autodiff_NORMALIZER_IMPL

} // namespace mim::plug::autodiff
