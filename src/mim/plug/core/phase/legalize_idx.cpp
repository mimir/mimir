#include "mim/plug/core/phase/legalize_idx.h"

#include <algorithm>
#include <bit>

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

namespace {

/// The size of the smallest of `I8`, `I16`, `I32`, and `I64` that holds the values of `Idx s`.
nat_t widen(nat_t s) { return Idx::bitwidth2size(std::max(8_n, std::bit_ceil(Idx::size2bitwidth(s)))); }

bool is_unsigned(icmp id) {
    switch (id) {
        case icmp::f:
        case icmp::e:
        case icmp::ug:
        case icmp::uge:
        case icmp::ul:
        case icmp::ule:
        case icmp::ne:
        case icmp::t: return true;
        default: return false;
    }
}

} // namespace

const Def* LegalizeIdx::zext(const Def* x) { return new_world().call(conv::u, new_world().lit_nat_0(), x); }

const Def* LegalizeIdx::sext(nat_t s, const Def* x) {
    auto& w  = new_world();
    auto z   = zext(x);
    auto neg = w.call(icmp::ug, Defs{z, w.lit_i64((s - 1) / 2)});
    return w.select(neg, w.call(wrap::sub, Mode::none, Defs{z, w.lit_i64(s)}), z);
}

const Def* LegalizeIdx::narrow(nat_t s, const Def* y) {
    return new_world().call(conv::u, new_world().lit_nat(widen(s)), y);
}

const Def* LegalizeIdx::mod_u(nat_t s, const Def* y) { return new_world().call<rem>(new_world().lit_nat(s), y); }

const Def* LegalizeIdx::mod_s(nat_t s, const Def* y) {
    auto& w   = new_world();
    auto zero = w.lit_i64(0);
    auto neg  = w.call(icmp::sl, Defs{y, zero});
    auto m    = mod_u(s, w.select(neg, w.call(wrap::sub, Mode::none, Defs{zero, y}), y));
    auto flip = w.select(w.call(icmp::e, Defs{m, zero}), zero, w.call(wrap::sub, Mode::none, Defs{w.lit_i64(s), m}));
    return narrow(s, w.select(neg, flip, m));
}

const Def* LegalizeIdx::mod_s_mem(nat_t s, const Def* r) {
    auto [m, v] = r->projs<2>();
    return new_world().tuple({m, mod_s(s, v)});
}

const Def* LegalizeIdx::add(nat_t s, const Def* a, const Def* b) {
    auto& w   = new_world();
    auto t    = w.call(wrap::add, Mode::none, Defs{a, b});
    auto over = w.call(icmp::uge, Defs{t, w.lit_i64(s)});
    // Two values below `s` only wrap around `I64` if `s` exceeds `2^63`; `t < a` catches that.
    if (s > 1_n << 63) over = w.call(bit2::or_, 0_n, Defs{w.call(icmp::ul, Defs{t, a}), over});
    return w.select(over, w.call(wrap::sub, Mode::none, Defs{t, w.lit_i64(s)}), t);
}

const Def* LegalizeIdx::sub(nat_t s, const Def* a, const Def* b) {
    auto& w = new_world();
    auto d  = w.call(wrap::sub, Mode::none, Defs{a, b});
    return w.select(w.call(icmp::ul, Defs{a, b}), w.call(wrap::add, Mode::none, Defs{d, w.lit_i64(s)}), d);
}

const Def* LegalizeIdx::mul(nat_t s, const Def* a, const Def* b) {
    return new_world().call<mulrem>(new_world().lit_nat(s), Defs{a, b});
}

const Def* LegalizeIdx::rewrite_imm_App(const App* app) {
    auto& w = new_world();

    if (auto s = isa_odd_idx(app)) return w.type_idx(widen(*s));

    if (auto op = Axm::isa<wrap>(app)) {
        if (auto s = isa_odd_idx(app->type())) {
            auto [a, b] = op->args<2>([this](const Def* d) { return zext(rewrite(d)); });
            // Without unsigned wrap-around, the result is below `s` anyway.
            if (auto m = Lit::isa(op->decurry()->decurry()->arg()); m && (*m & nat_t(Mode::nuw)))
                return narrow(*s, w.call(op.id(), Mode::nuw, Defs{a, b}));
            switch (op.id()) {
                case wrap::add: return narrow(*s, add(*s, a, b));
                case wrap::sub: return narrow(*s, sub(*s, a, b));
                case wrap::mul: return narrow(*s, mul(*s, a, b));
                case wrap::shl: {
                    auto pow = mod_u(*s, w.call(wrap::shl, Mode::none, Defs{w.lit_i64(1), b}));
                    return narrow(*s, mul(*s, a, pow));
                }
            }
        }
    } else if (auto op = Axm::isa<conv>(app)) {
        auto x   = op->arg();
        auto src = Idx::isa_lit(x->type());
        if (auto d = isa_odd_idx(app->type())) {
            if (op.id() == conv::u) {
                if (src && *src != 0 && *src <= *d) return narrow(*d, rewrite(x));
                return narrow(*d, mod_u(*d, zext(rewrite(x))));
            }
            auto y = isa_odd_idx(x->type()) ? sext(*src, rewrite(x)) : w.call(conv::s, w.lit_nat_0(), rewrite(x));
            return mod_s(*d, y);
        }
        if (auto s = isa_odd_idx(x->type()); s && op.id() == conv::s)
            return w.call(conv::s, rewrite(Idx::isa(app->type())), sext(*s, rewrite(x)));
    } else if (Axm::isa<bitcast>(app) || Axm::isa<idx>(app)) {
        if (auto d = isa_odd_idx(app->type()))
            return narrow(*d, mod_u(*d, w.call<bitcast>(w.type_i64(), rewrite(app->arg()))));
    } else if (auto op = Axm::isa<icmp>(app)) {
        auto [a, b] = op->args<2>();
        if (auto s = isa_odd_idx(a->type())) {
            auto ext = [&](const Def* x) { return is_unsigned(op.id()) ? zext(rewrite(x)) : sext(*s, rewrite(x)); };
            return w.call(op.id(), Defs{ext(a), ext(b)});
        }
    } else if (auto op = Axm::isa<shr>(app)) {
        if (auto s = isa_odd_idx(app->type()); s && op.id() == shr::a) {
            auto [a, b] = op->args<2>();
            return mod_s(*s, w.call(shr::a, Defs{sext(*s, rewrite(a)), zext(rewrite(b))}));
        }
    } else if (auto op = Axm::isa<extrema>(app)) {
        if (auto s = isa_odd_idx(app->type()); s && (op.id() == extrema::smin || op.id() == extrema::smax)) {
            auto [a, b] = op->args<2>();
            return mod_s(*s, w.call(op.id(), Defs{sext(*s, rewrite(a)), sext(*s, rewrite(b))}));
        }
    } else if (auto op = Axm::isa<abs>(app)) {
        auto [m, x] = op->args<2>();
        if (auto s = isa_odd_idx(x->type())) return mod_s_mem(*s, w.call<abs>(Defs{rewrite(m), sext(*s, rewrite(x))}));
    } else if (auto op = Axm::isa<div>(app)) {
        auto [m, xy] = op->args<2>();
        auto [x, y]  = xy->projs<2>();
        if (auto s = isa_odd_idx(x->type()); s && (op.id() == div::sdiv || op.id() == div::srem))
            return mod_s_mem(*s,
                             w.call(op.id(), Defs{rewrite(m), w.tuple({sext(*s, rewrite(x)), sext(*s, rewrite(y))})}));
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LegalizeIdx::index_type(const Def* type) {
    auto& w = new_world();
    if (auto s = isa_odd_idx(type)) return w.type_idx(*s);
    if (auto sigma = type->isa_imm<Sigma>())
        return w.sigma(DefVec(sigma->ops(), [this](const Def* op) { return index_type(op); }));
    if (auto arr = type->isa_imm<Arr>()) return w.arr(rewrite(arr->arity()), index_type(arr->body()));
    return rewrite(type);
}

const Def* LegalizeIdx::index(const Def* index) { return coerce(index_type(index->type()), rewrite(index)); }

const Def* LegalizeIdx::rewrite_imm_Extract(const Extract* extract) {
    return new_world().extract(rewrite(extract->tuple()), index(extract->index()));
}

const Def* LegalizeIdx::rewrite_imm_Insert(const Insert* insert) {
    return new_world().insert(rewrite(insert->tuple()), index(insert->index()), rewrite(insert->value()));
}

const Def* LegalizeIdx::coerce_leaf(const Def* type, const Def* val) {
    if (auto size = Idx::isa(type); size && Idx::isa(val->type()) && (isa_odd_idx(type) || isa_odd_idx(val->type())))
        return new_world().call(conv::u, size, val);
    return nullptr;
}

} // namespace mim::plug::core::phase
