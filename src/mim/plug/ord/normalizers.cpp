#include <fe/assert.h>

#include <mim/world.h>

#include <mim/plug/option/option.h>

#include "mim/plug/ord/ord.h"

namespace mim::plug::ord {

template<init id>
const Def* normalize_init(const Def* type, const Def* callee, const Def* arg) {
    auto& world = type->world();
    return world.raw_app(type, callee, arg);
}

template<size>
const Def* normalize_size(const Def*, const Def*, const Def* arg) {
    if (auto init = Axm::isa<ord::init>(arg)) return init->decurry()->arg();
    return nullptr;
}

const Def* normalize_get(const Def*, const Def* callee, const Def* arg) {
    auto& w       = arg->world();
    auto [map, k] = arg->projs<2>();
    auto V        = callee->as<App>()->arg();

    if (auto init = Axm::isa<ord::init>(map)) {
        if (auto n = Lit::isa(init->decurry()->arg())) {
            auto kvs   = init->arg();
            auto opt_V = w.call<option::Opt>(V);
            for (auto kv : kvs->projs(*n))
                if (auto [key, val] = kv->projs<2>(); key == k) return w.inj(opt_V, val);
            // FIXME a closed kvs doesn't rule out an open k matching at runtime.
            if (kvs->is_closed()) return w.inj(opt_V, w.tuple(Defs{}));
        }
    }

    return nullptr;
}

template<contains id>
const Def* normalize_contains(const Def*, const Def*, const Def* arg) {
    auto& w     = arg->world();
    auto [c, k] = arg->projs<2>();

    if (auto init = Axm::isa<ord::init>(c)) {
        if (auto n = Lit::isa(init->decurry()->arg())) {
            auto kvs = init->arg();
            for (auto kv : kvs->projs(*n)) {
                auto key = id == contains::map ? kv->proj(2, 0) : kv;
                if (key == k) return w.lit_tt();
            }
            // FIXME a closed kvs doesn't rule out an open k matching at runtime.
            return kvs->is_closed() ? w.lit_ff() : nullptr;
        }

        if (auto pack = init->arg()->isa_imm<Pack>()) w.log().w("Pack not yet implemented: {}", pack);
    }

    return nullptr;
}

template<insert id>
const Def* normalize_insert(const Def* type, const Def*, const Def* arg) {
    auto& w       = type->world();
    auto [ms, kv] = arg->projs<2>();

    if (auto init = Axm::isa<ord::init>(ms)) {
        auto [n, kvs] = init->uncurry_args<2>();
        if (auto l = Lit::isa(n)) {
            auto key     = id == ord::insert::map ? kv->proj(2, 0) : kv;
            auto new_ops = DefVec();
            bool updated = false;
            for (auto cur : kvs->projs(*l)) {
                auto cur_key = id == ord::insert::map ? cur->proj(2, 0) : cur;
                if (key == cur_key) updated = true;
                new_ops.emplace_back(key == cur_key ? kv : cur);
            }
            if (!updated) new_ops.emplace_back(kv);
            return w.app(w.app(init->decurry()->callee(), w.lit_nat(new_ops.size())), new_ops);
        }

        if (auto pack = init->arg()->isa_imm<Pack>()) w.log().w("Pack not yet implemented: {}", pack);
    }

    return {};
}

MIM_ord_NORMALIZER_IMPL

} // namespace mim::plug::ord
