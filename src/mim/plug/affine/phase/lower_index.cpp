#include "mim/plug/affine/phase/lower_index.h"

#include <cstdint>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <ranges>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>
#include <mim/plug/refly/refly.h>

#include "mim/plug/affine/affine.h"

namespace mim::plug::affine::phase {

namespace {

/// Bound on the magnitude of a proven range: below it, sums and products of proven ranges cannot wrap an i64.
constexpr int64_t Limit = int64_t{1} << 61;

std::optional<int64_t> const_val(const Def* d) {
    if (auto bc = Axm::isa<core::bitcast>(d)) d = bc->arg();
    if (auto lit = Lit::isa(d); lit && *lit <= u64(Limit)) return int64_t(*lit);
    return {};
}

/// Splits x * k into (x, k).
std::optional<std::pair<const Def*, int64_t>> as_scaled(const Def* d) {
    auto wrap = Axm::isa<core::wrap>(d);
    if (!wrap || wrap.id() != core::wrap::mul) return {};
    auto [a, b] = wrap->arg()->projs<2>();
    if (auto k = const_val(b)) return std::pair{a, *k};
    if (auto k = const_val(a)) return std::pair{b, *k};
    return {};
}

/// True when @p d is a multiple of @p c for every value of its free indices.
bool divides(const Def* d, int64_t c) {
    if (c == 0) return false;
    if (auto k = const_val(d)) return *k % c == 0;
    if (auto s = as_scaled(d)) return s->second % c == 0 || divides(s->first, c);
    auto wrap = Axm::isa<core::wrap>(d);
    if (!wrap || (wrap.id() != core::wrap::add && wrap.id() != core::wrap::sub)) return false;
    auto [a, b] = wrap->arg()->projs<2>();
    return divides(a, c) && divides(b, c);
}

const Def* times(World& w, const Def* d, int64_t k) {
    return k == 1 ? d : w.call(core::wrap::mul, core::Mode::none, Defs{d, w.lit_i64(u64(k))});
}

/// A linearized index scales whole sums by a stride, so the folds must not depend on how the sum was nested.
void flatten_add(World& w, const Def* d, int64_t k, DefVec& terms) {
    if (auto wrap = Axm::isa<core::wrap>(d); wrap && wrap.id() == core::wrap::add) {
        auto [a, b] = wrap->arg()->projs<2>();
        flatten_add(w, a, k, terms);
        flatten_add(w, b, k, terms);
    } else if (auto s = as_scaled(d); s && s->second != 0 && std::abs(k) <= Limit / std::abs(s->second)) {
        flatten_add(w, s->first, k * s->second, terms);
    } else {
        terms.emplace_back(times(w, d, k));
    }
}

/// @p d divided by @p c, for a @p d that divides established is an exact multiple.
const Def* exact_div(World& w, const Def* d, int64_t c) {
    if (c == 1) return d;
    if (auto k = const_val(d)) return w.lit_i64(u64(*k / c));
    if (auto s = as_scaled(d)) {
        auto [x, k] = *s;
        return k % c == 0 ? times(w, x, k / c) : times(w, exact_div(w, x, c), k);
    }
    auto wrap   = Axm::isa<core::wrap>(d); // add or sub, both of whose sides divide
    auto [a, b] = wrap->arg()->projs<2>();
    return w.call(wrap.id(), core::Mode::none, Defs{exact_div(w, a, c), exact_div(w, b, c)});
}

const Def* sum(World& w, Defs terms) {
    const Def* acc = nullptr;
    for (auto t : terms)
        acc = acc ? w.call(core::wrap::add, core::Mode::none, Defs{acc, t}) : t;
    return acc ? acc : w.lit_i64(0);
}

} // namespace

std::optional<LowerIndex::Range> LowerIndex::range_of(const Def* d) const {
    auto bounded = [](int64_t lo, int64_t hi) -> std::optional<Range> {
        if (lo < -Limit || hi > Limit) return {};
        return Range{lo, hi};
    };

    if (auto k = const_val(d)) return Range{*k, *k};

    if (auto ex = d->isa<Extract>()) {
        auto i = Lit::isa(ex->index());
        if (ex->tuple() == dom_.idxs && i && *i < dom_.extents.size())
            if (auto n = dom_.extents[*i]; n && *n > 0) return Range{0, *n - 1};

        if (auto div = Axm::isa<core::div>(ex->tuple()); div && i == 1) {
            // `core.div` takes `(mem, (x, c))`.
            auto [x, c] = div->arg()->proj(2, 1)->projs<2>();
            auto k      = const_val(c);
            if (!k || *k <= 0) return {};
            // `urem` is bounded by its divisor whatever its operand; `udiv` only by a non-negative operand's range.
            if (div.id() == core::div::urem) return Range{0, *k - 1};
            if (auto l = range_of(x); l && l->lo >= 0 && div.id() == core::div::udiv)
                return Range{l->lo / *k, l->hi / *k};
        }
    }

    if (auto wrap = Axm::isa<core::wrap>(d)) {
        auto [a, b] = wrap->arg()->projs<2>();
        auto l = range_of(a), r = range_of(b);
        if (!l || !r) return {};
        switch (wrap.id()) {
            case core::wrap::add: return bounded(l->lo + r->lo, l->hi + r->hi);
            case core::wrap::sub: return bounded(l->lo - r->hi, l->hi - r->lo);
            case core::wrap::mul: {
                std::array cands{std::pair{l->lo, r->lo}, std::pair{l->lo, r->hi}, std::pair{l->hi, r->lo},
                                 std::pair{l->hi, r->hi}};
                for (auto [x, y] : cands)
                    if (x != 0 && std::abs(y) > Limit / std::abs(x)) return {};
                auto prods = cands | std::views::transform([](auto p) { return p.first * p.second; });
                return bounded(std::ranges::min(prods), std::ranges::max(prods));
            }
            default: break;
        }
    }

    return {};
}

LowerIndex::Split LowerIndex::split(const Def* x, int64_t c) {
    DefVec terms;
    flatten_add(new_world(), x, 1, terms);

    Split split;
    std::optional<Range> rest = Range{0, 0};
    for (auto t : terms) {
        if (divides(t, c)) {
            split.even.emplace_back(t);
        } else {
            split.rest.emplace_back(t);
            auto r = range_of(t);
            if (rest && r && std::abs(rest->lo) <= Limit && std::abs(rest->hi) <= Limit)
                rest = Range{rest->lo + r->lo, rest->hi + r->hi};
            else
                rest = std::nullopt;
        }
    }
    split.carries = !rest || rest->lo < 0 || rest->hi >= c;
    return split;
}

/// x floordiv c without a division, or nullptr when the range analysis cannot justify one.
const Def* LowerIndex::fold_udiv(const Def* x, int64_t c) {
    auto& w = new_world();
    if (c <= 0) return nullptr;
    if (c == 1) return x;
    if (auto k = const_val(x)) return w.lit_i64(u64(*k / c));

    // The carrier wraps, so a fold needs all of x proven in range; an unknown term keeps the division.
    auto r = range_of(x);
    if (!r || r->lo < 0) return nullptr;
    if (r->hi < c) return w.lit_i64(0);

    auto s = split(x, c);
    if (s.even.empty() || s.carries) return nullptr;
    return sum(w, DefVec(s.even.size(), [&](size_t i) { return exact_div(w, s.even[i], c); }));
}

/// x mod c without a division, or nullptr when the range analysis cannot justify one.
const Def* LowerIndex::fold_urem(const Def* x, int64_t c) {
    auto& w = new_world();
    if (c <= 0) return nullptr;
    if (c == 1) return w.lit_i64(0);
    if (auto k = const_val(x)) return w.lit_i64(u64(*k % c));

    auto r = range_of(x);
    if (!r || r->lo < 0) return nullptr;
    if (r->hi < c) return x;
    if (divides(x, c)) return w.lit_i64(0);

    auto s = split(x, c);
    return s.even.empty() || s.carries ? nullptr : sum(w, s.rest);
}

const Def* LowerIndex::rewrite(const Def* def) {
    // The opaque affine index type lowers to the wide `Idx 0` (i64) carrier.
    if (Axm::isa<affine::Index>(def)) return new_world().type_i64();
    return RWPhase::rewrite(def);
}

const Def* LowerIndex::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    auto& w = new_world();

    // Emits `core.div.<op> (mem_, (x, c))` on the `Idx 0` carrier, advancing the threaded mem and yielding the value.
    auto div = [&](core::div op, const Def* x, const Def* c) -> const Def* {
        auto [m, v] = w.call(op, Defs{mem_, w.tuple({x, c})})->projs<2>();
        mem_        = m;
        return v;
    };

    // The affine index algebra is computed on the wide `Idx 0` carrier with wrap-around (`Mode::none`) arithmetic, so
    // that negation/subtraction are correct via two's complement; the boundary `affine.map` casts in/out with
    // `core.conv.u`.

    // affine.lit n ↦ the `Nat` n reinterpreted as an `Idx 0`.
    if (Axm::isa<affine::lit>(app)) return w.call<core::bitcast>(w.type_i64(), rewrite(app->arg()));

    if (auto op = Axm::isa<affine::op>(app)) {
        switch (op.id()) {
            case affine::op::add: {
                auto [a, b] = rewrite(app->arg())->projs<2>();
                return w.call(core::wrap::add, core::Mode::none, Defs{a, b});
            }
            case affine::op::sub: {
                auto [a, b] = rewrite(app->arg())->projs<2>();
                return w.call(core::wrap::sub, core::Mode::none, Defs{a, b});
            }
            case affine::op::neg: {
                auto a = rewrite(app->arg());
                return w.call(core::wrap::sub, core::Mode::none, Defs{w.lit(w.type_i64(), 0), a});
            }
            case affine::op::mul: {
                fe::throwf("`affine.op.mul` should have been rewritten to `affine.semiop.mul` and then to `core.mul`");
            }
        }
    }

    if (auto semiop = Axm::isa<affine::semiop>(app)) {
        auto [x, c] = rewrite(app->arg())->projs<2>();
        switch (semiop.id()) {
            case affine::semiop::mul: {
                if (Axm::isa<refly::check>(c))
                    fe::throwf("`affine.semiop.mul` called with non-constant second argument");
                // `c` is a `Nat` constant; reinterpret it on the `Idx 0` carrier.
                return w.call(core::wrap::mul, core::Mode::none, Defs{x, w.call<core::bitcast>(w.type_i64(), c)});
            }
            case affine::semiop::floordiv: {
                auto c_idx = w.call<core::bitcast>(w.type_i64(), c);
                if (auto k = const_val(c_idx))
                    if (auto folded = fold_udiv(x, *k)) return folded;
                return div(core::div::udiv, x, c_idx);
            }
            case affine::semiop::rem: {
                auto c_idx = w.call<core::bitcast>(w.type_i64(), c);
                if (auto k = const_val(c_idx))
                    if (auto folded = fold_urem(x, *k)) return folded;
                return div(core::div::urem, x, c_idx);
            }
            case affine::semiop::ceildiv: {
                auto c_idx = w.call<core::bitcast>(w.type_i64(), c);
                // ceildiv(x, c) = (x + (c - 1)) / c  (unsigned, on the Idx 0 carrier)
                auto c_1 = w.call(core::wrap::sub, core::Mode::none, Defs{c_idx, w.lit(w.type_i64(), 1)});
                return div(core::div::udiv, w.call(core::wrap::add, core::Mode::none, Defs{x, c_1}), c_idx);
            }
        }
    }

    // Row-major suffix-product strides of a shape `s` («n; Nat»): `strides#k = ∏_{j>k} s#j` (on `Idx 0`, via
    // `core.nat`).
    auto strides = [&](const Def* s, size_t n) {
        DefVec str(n);
        if (n) str[n - 1] = w.lit_nat(1);
        for (size_t k = n - 1; k-- != 0;)
            str[k] = w.call(core::nat::mul, Defs{str[k + 1], s->proj(n, k + 1)});
        for (size_t k = 0; k != n; ++k)
            str[k] = w.call<core::bitcast>(w.type_i64(), str[k]);
        return str;
    };

    // affine.linearize (idxs, s) ↦ Σ_k idxs#k · strides#k  (on the `Idx 0` carrier).
    if (Axm::isa<affine::linearize>(app)) {
        auto [idxs, s] = rewrite(app->arg())->projs<2>();
        auto xs        = idxs->projs();
        auto str       = strides(s, xs.size());
        const Def* lin = w.lit(w.type_i64(), 0);
        for (size_t k = 0; k != xs.size(); ++k) {
            auto term = w.call(core::wrap::mul, core::Mode::none, Defs{xs[k], str[k]});
            lin       = w.call(core::wrap::add, core::Mode::none, Defs{lin, term});
        }
        return lin;
    }

    // affine.delinearize (lin, s) ↦ (lin floordiv strides#d) mod s#d for each d  (on the `Idx 0` carrier).
    if (Axm::isa<affine::delinearize>(app)) {
        auto [lin, s] = rewrite(app->arg())->projs<2>();
        auto m        = s->num_projs();
        auto str      = strides(s, m);
        return w.tuple(DefVec(m, [&](size_t d) {
            const Def* q = nullptr;
            if (auto k = const_val(str[d])) q = fold_udiv(lin, *k);
            if (!q) q = div(core::div::udiv, lin, str[d]);

            auto ext = w.call<core::bitcast>(w.type_i64(), s->proj(m, d));
            if (auto k = const_val(ext))
                if (auto folded = fold_urem(q, *k)) return folded;
            return div(core::div::urem, q, ext);
        }));
    }

    // affine.map f idxs mem ↦ widen idxs to `Idx 0`, inline f (advancing the threaded mem through any div), and narrow
    // each result back to its target `Idx (sout#j)`; returns `(mem', narrowed)`.
    if (Axm::isa<affine::map>(app)) {
        // Extract f/idxs/sout from the *old* callee; we inline f's body at this call site rather than rewriting it into
        // a standalone lam, since its body may reference the threaded mem (from the divs) and would otherwise be open.
        auto [mn, sinout, f, idxs, _] = app->callee()->as<App>()->uncurry_args<5>();
        auto [sin, sout]              = sinout->projs<2>();

        auto __     = fe::Restore(mem_);
        auto mem    = rewrite(app->arg()); // the `affine.map`'s mem operand
        auto ins    = rewrite(idxs)->projs();
        auto lifted = w.tuple(DefVec(ins.size(), [&](size_t i) { return w.call(core::conv::u, w.lit_i64(), ins[i]); }));
        auto f_lam  = f->isa_mut<Lam>();

        Lam* idx_map_lam = nullptr;
        Lam* rw_idx_lam  = nullptr;
        if (auto it = specialized_[f_lam].find(sin); it != specialized_[f_lam].end()) {
            idx_map_lam = it->second;
        } else if (auto idx_lam = lookup(f_lam)) {
            // Anything already carrying a mem was rewritten for a *different* `sin` and gets its own specialization
            // below; anything else was rewritten as part of an annex and needs the mem parameter added.
            if (auto mut = idx_lam->isa_mut<Lam>();
                !(mut && mut->num_vars() == 2 && mut->var(2, 0)->type() == mem->type()))
                rw_idx_lam = idx_lam->as_mut<Lam>();
        }
        if (!idx_map_lam) {
            auto lam_pi = rewrite(f_lam->type())->as<Pi>();

            idx_map_lam              = w.mut_lam(w.pi({mem->type(), lam_pi->dom()}, {mem->type(), lam_pi->codom()}));
            specialized_[f_lam][sin] = idx_map_lam;
            if (!lookup(f_lam)) map(f_lam, idx_map_lam);

            push();
            map(f_lam->var(), idx_map_lam->var(2, 1));
            for (size_t i = 0; i != f_lam->num_vars(); ++i)
                map(f_lam->var(f_lam->num_vars(), i), idx_map_lam->var(2, 1)->proj(f_lam->num_vars(), i));
            mem_ = idx_map_lam->var(2, 0);

            auto n           = sin->num_projs();
            auto extents     = fe::Vector<std::optional<int64_t>>(n, [&](size_t i) -> std::optional<int64_t> {
                if (auto e = Lit::isa(sin->proj(n, i)); e && *e <= u64(Limit)) return int64_t(*e);
                return {};
            });
            auto restore_dom = fe::Restore(dom_, Domain{idx_map_lam->var(2, 1), std::move(extents)});

            auto get_body = [&]() -> const Def* {
                if (rw_idx_lam) return rw_idx_lam->reduce_body(idx_map_lam->var(2, 1));
                return rewrite(f_lam->body());
            };
            idx_map_lam->set(true, w.tuple({mem_, get_body()}))->set(f_lam->dbg_key());
            pop();
        }

        auto outs = w.app(idx_map_lam, {mem, lifted});

        auto sout_n   = rewrite(sout);
        auto n_out    = sout_n->num_projs();
        auto narrowed = w.tuple(DefVec(n_out, [&](size_t j) {
            return w.call(core::conv::u, sout_n->proj(n_out, j), outs->proj(2, 1)->proj(n_out, j));
        }));

        return w.tuple({outs->proj(2, 0), narrowed});
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::affine::phase
