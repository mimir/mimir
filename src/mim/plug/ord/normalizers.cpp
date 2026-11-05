#include <algorithm>
#include <optional>

#include <fe/assert.h>

#include <mim/world.h>

#include <mim/plug/option/option.h>

#include "mim/plug/ord/ord.h"

namespace mim::plug::ord {

namespace {

/// Decides whether the keys @p k1 and @p k2 are equal; `std::nullopt` means "don't know".
/// Distinct literals are distinct keys, as the key's `lt` is assumed to be a strict total order.
std::optional<bool> key_eq(const Def* k1, const Def* k2) {
    if (k1 == k2) return true;
    if (k1->isa<Lit>() && k2->isa<Lit>()) return false;
    return {};
}

} // namespace

template<init id>
const Def* normalize_init(const Def* type, const Def* callee, const Def* arg) {
    auto& w = type->world();
    if (auto n = Lit::isa(callee->as<App>()->arg())) {
        auto key = [](const Def* kv) { return id == init::map ? kv->proj(2, 0) : kv; };
        auto kvs = DefVec();
        for (auto kv : arg->projs(*n))
            if (auto i = std::ranges::find_if(kvs, [&](const Def* cur) { return key(cur) == key(kv); }); i != kvs.end())
                *i = kv;
            else
                kvs.emplace_back(kv);
        if (kvs.size() != *n) return w.app(w.app(callee->as<App>()->callee(), w.lit_nat(kvs.size())), kvs);
    }
    return w.raw_app(type, callee, arg);
}

template<size>
const Def* normalize_size(const Def*, const Def*, const Def* arg) {
    if (auto init = Axm::isa<ord::init>(arg)) return init->decurry()->arg();
    return nullptr;
}

const Def* normalize_get(const Def* type, const Def* callee, const Def* arg) {
    auto& w       = arg->world();
    auto [map, k] = arg->projs<2>();

    if (auto init = Axm::isa<ord::init>(map)) {
        if (auto n = Lit::isa(init->decurry()->arg())) {
            auto kvs = init->arg();
            for (auto kv : kvs->projs(*n))
                if (auto [key, val] = kv->projs<2>(); key == k) return w.inj(type, option::Opt_ctor::Some, val);
            // FIXME a closed kvs doesn't rule out an open k matching at runtime.
            if (kvs->is_closed()) return w.inj(type, option::Opt_ctor::None, w.tuple());
        }
    }

    // get(insert(m, (k', v)), k) = Some v       if k' = k
    //                            = get(m, k)    if k' ≠ k
    if (auto ins = Axm::isa(insert::map, map)) {
        auto [m, kv] = ins->args<2>();
        auto [k_, v] = kv->projs<2>();
        if (auto eq = key_eq(k_, k))
            return *eq ? w.inj(type, option::Opt_ctor::Some, v) : w.app(callee, w.tuple({m, k}));
    }

    return nullptr;
}

template<contains id>
const Def* normalize_contains(const Def*, const Def* callee, const Def* arg) {
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

    // contains(insert(m, k'), k) = tt              if k' = k
    //                            = contains(m, k)  if k' ≠ k
    if (auto ins = Axm::isa(id == contains::map ? insert::map : insert::set, c)) {
        auto [m, kv] = ins->args<2>();
        auto key     = id == contains::map ? kv->proj(2, 0) : kv;
        if (auto eq = key_eq(key, k)) return *eq ? w.lit_tt() : w.app(callee, w.tuple({m, k}));
    }

    return nullptr;
}

template<insert id>
const Def* normalize_insert(const Def* type, const Def* callee, const Def* arg) {
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

    // insert(insert(m, k'), k) = insert(m, k)                 if k' = k
    //                          = insert(insert(m, k), k')     if k' ≠ k and k < k' (canonical order of literal keys)
    if (auto ins = Axm::isa(id, ms)) {
        auto [m, kv_] = ins->args<2>();
        auto key      = [](const Def* kv) { return id == ord::insert::map ? kv->proj(2, 0) : kv; };
        if (auto eq = key_eq(key(kv_), key(kv))) {
            if (*eq) return w.app(callee, w.tuple({m, kv}));
            if (*Lit::isa(key(kv)) < *Lit::isa(key(kv_)))
                return w.app(callee, w.tuple({w.app(callee, w.tuple({m, kv})), kv_}));
        }
    }

    return {};
}

MIM_ord_NORMALIZER_IMPL

} // namespace mim::plug::ord
