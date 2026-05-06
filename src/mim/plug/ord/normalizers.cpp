#include "mim/world.h"

#include "mim/plug/option/option.h"
#include "mim/plug/ord/ord.h"

#include "fe/assert.h"

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
        if (auto tuple = init->arg()->isa<Tuple>()) {
            for (auto kv : tuple->ops()) {
                if (kv->proj(2, 0) == k) {
                    auto val   = kv->proj(2, 1);
                    auto opt_V = w.call<option::Opt>(V);
                    return w.inj(opt_V, val);
                }
            }
            if (tuple->is_closed()) {
                auto opt_V = w.call<option::Opt>(V);
                return w.inj(opt_V, w.tuple(Defs{}));
            };
        }
    }

    return nullptr;
}

template<contains id>
const Def* normalize_contains(const Def*, const Def*, const Def* arg) {
    auto& w     = arg->world();
    auto [c, k] = arg->projs<2>();

    if (auto init = Axm::isa<ord::init>(c)) {
        if (auto tuple = init->arg()->isa<Tuple>()) {
            for (auto kv : tuple->ops()) {
                auto key = id == contains::map ? kv->proj(2, 0) : kv;
                if (key == k) return w.lit_tt();
            }
            return tuple->is_closed() ? w.lit_ff() : nullptr;
        }

        if (auto pack = init->arg()->isa_imm<Pack>()) w.WLOG("packs not yet implemented: {}", pack);
    }

    return nullptr;
}

template<insert id>
const Def* normalize_insert(const Def* type, const Def*, const Def* arg) {
    auto& w       = type->world();
    auto [ms, kv] = arg->projs<2>();

    if (auto init = Axm::isa<ord::init>(ms)) {
        auto [K, V, n, arg] = init->uncurry_args<4>();
        if (auto tuple = arg->isa<Tuple>()) {
            K = K ? K : V;
            if (auto l = Lit::isa(n)) {
                auto new_ops = DefVec();
                bool updated = false;
                for (size_t i = 0, e = *l; i != e; ++i) {
                    auto key = id == ord::insert::map ? kv->proj(2, 0) : kv;
                    auto cur = tuple->proj(e, i);
                    if (id == ord::insert::map) cur = cur->proj(2, 0);
                    if (key == cur) {
                        updated = true;
                        new_ops.emplace_back(kv);
                    } else {
                        new_ops.emplace_back(tuple->proj(e, i));
                    }
                }

                if (!updated) new_ops.emplace_back(kv);

                // return w.call(id, lt, Defs(new_ops));
                auto insert = id == ord::insert::map ? ord::init::map : ord::init::set;
                auto new_n  = w.lit_nat(new_ops.size());
                auto app    = w.app(w.annex(insert), K);
                if (id == ord::insert::map) app = w.app(app, V);

                return w.app(w.app(app, new_n), new_ops);
            }
        }

        if (auto pack = init->arg()->isa_imm<Pack>()) w.WLOG("packs not yet implemented: {}", pack);
    }

    return {};
}

MIM_ord_NORMALIZER_IMPL

} // namespace mim::plug::ord
