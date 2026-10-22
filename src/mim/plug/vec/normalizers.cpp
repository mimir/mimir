#include <fe/bitset.h>

#include <mim/tuple.h>
#include <mim/world.h>

#include <mim/plug/core/core.h>

#include "mim/plug/vec/vec.h"

namespace mim::plug::vec {

namespace {

/// The elements of the `n`-vector @p vec, if they are known.
std::optional<DefVec> elems(const Def* n, const Def* vec) {
    auto l = Lit::isa(n);
    if (l == 0) return DefVec();
    if (l == 1) return DefVec{vec}; // `«1; E»` ≡ `E`, even if `E` is itself an aggregate
    if (auto tuple = vec->isa<Tuple>()) return DefVec(tuple->ops().begin(), tuple->ops().end());
    if (auto seq = vec->isa<Seq>(); seq && !seq->shape().is_fused())
        if (auto a = Lit::isa(seq->arity())) return seq->projs(*a);
    return {};
}

} // namespace

template<fold id>
const Def* normalize_fold(const Def*, const Def* c, const Def* arg) {
    auto& w         = c->world();
    auto [_, n, f]  = App::uncurry_args<3>(c);
    auto [acc, vec] = arg->projs<2>();
    if constexpr (id == fold::r) std::swap(acc, vec);

    auto es = elems(n, vec);
    if (!es) return nullptr;

    if constexpr (id == fold::l)
        for (auto e : *es)
            acc = w.app(f, {acc, e});
    else // fold::r
        for (auto e : *es | std::views::reverse)
            acc = w.app(f, {e, acc});
    return acc;
}

const Def* normalize_zip(const Def* type, const Def* c, const Def* arg) {
    if (arg->is_open()) return {};
    auto& w           = type->world();
    auto [ni_n, _, f] = App::uncurry_args<3>(c);
    auto [ni, n]      = ni_n->projs<2>([](const Def* def) { return Lit::isa(def); });

    if (!ni || !n) return {};
    if (ni >= w.flags().scalarize_threshold || n >= w.flags().scalarize_threshold) return {};

    auto res = fe::Vector<const Def*, 32>(*n);
    auto tup = fe::Vector<const Def*, 32>(*ni);

    for (size_t j = 0; j != n; ++j) {
        for (size_t i = 0; i != ni; ++i)
            tup[i] = arg->proj(*ni, i)->proj(*n, j);

        res[j] = w.app(f, tup);
    }

    return w.tuple(res);
}

template<scan id>
const Def* normalize_scan(const Def*, const Def* c, const Def* vec) {
    auto& w        = c->world();
    auto [_, n, p] = App::uncurry_args<3>(c);
    if (Lit::isa(n) == 1) return w.app(p, vec);

    // All elements of `‹s; x›` are `x`, so besides `p x` only whether `s` is empty matters.
    if (auto pack = vec->isa_imm<Pack>()) {
        auto shape = pack->shape();
        auto r     = shape.rank();
        if (!r) return nullptr;

        auto res = w.app(p, pack->body());
        for (nat_t i = 0; i != *r; ++i) {
            // World::seq folds literal-`0` extents away.
            if (Lit::isa(shape[i])) continue;
            auto cmp = w.call(id == scan::exists ? core::ncmp::ne : core::ncmp::e, Defs{shape[i], w.lit_nat_0()});
            res      = w.call(id == scan::exists ? core::bit2::and_ : core::bit2::or_, 0_n, Defs{cmp, res});
        }
        return res;
    }

    auto es = elems(n, vec);
    if (!es) return nullptr;

    const Def* acc = w.lit_bool(id != scan::exists);
    for (auto e : *es)
        acc = w.call(id == scan::exists ? core::bit2::or_ : core::bit2::and_, 0_n, Defs{acc, w.app(p, e)});
    return acc;
}

const Def* normalize_is_unique(const Def*, const Def* callee, const Def* vec) {
    auto& w = vec->world();
    auto n  = Lit::isa(callee->as<App>()->arg());
    if (n && *n <= 1) return w.lit_tt();

    if (auto tuple = vec->isa<Tuple>(); tuple && n) {
        auto seen = DefSet();
        for (auto op : tuple->ops()) {
            auto [_, ins] = seen.emplace(op);
            if (!ins) return w.lit_ff();
        }
        return tuple->is_closed() ? w.lit_tt() : nullptr;
    }

    if (auto pack = vec->isa_imm<Pack>()) {
        if (Lit::isa(pack->arity())) return w.lit_ff();
    }

    return nullptr;
}

const Def* normalize_cat(const Def*, const Def* callee, const Def* arg) {
    auto [a, b] = arg->projs<2>();
    auto [n, m] = callee->as<App>()->decurry()->args<2>([](auto def) { return Lit::isa(def); });
    if (n && *n == 0) return b;
    if (m && *m == 0) return a;
    return n && m ? Tuple::cat(*n, *m, a, b) : nullptr;
}

const Def* normalize_diff(const Def* type, const Def* c, const Def* arg) {
    if (auto arr = type->isa<Arr>()) {
        if (arr->arity()->isa<Bot>()) return nullptr; // ack error
    }

    auto& w        = type->world();
    auto callee    = c->as<App>();
    auto [n, m]    = callee->args<2>([](auto def) { return Lit::isa(def); });
    auto [vec, is] = arg->projs<2>();

    if (!n || !m) return nullptr;
    if (n == 1 && m == 1) return w.tuple();

    if (auto tup_vec = vec->isa<Tuple>()) {
        if (auto tup_is = is->isa<Tuple>(); tup_is && tup_is->is_closed()) {
            auto defs = DefVec();
            auto drop = fe::Bitset();
            for (auto opi : tup_is->ops())
                drop.set(Lit::as(opi));

            for (size_t i = 0, e = tup_vec->num_ops(); i != e; ++i)
                if (!drop.test(i)) defs.emplace_back(tup_vec->op(i));
            return w.tuple(defs);
        }
        if (auto lit_is = Lit::isa(is)) {
            auto defs = DefVec();

            for (size_t i = 0, e = tup_vec->num_ops(); i != e; ++i)
                if (i != lit_is) defs.emplace_back(tup_vec->op(i));
            return w.tuple(defs);
        }
    }

    if (auto tup_pack = vec->isa_imm<Pack>()) return w.pack(*n - *m, tup_pack->body());

    return nullptr;
}

MIM_vec_NORMALIZER_IMPL

} // namespace mim::plug::vec
