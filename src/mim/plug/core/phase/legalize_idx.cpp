#include "mim/plug/core/phase/legalize_idx.h"

#include <algorithm>
#include <bit>

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

namespace {

/// The literal size of the `Idx` @p type if it is no `Bool`, `I8`, `I16`, `I32`, or `I64`.
std::optional<nat_t> odd(const Def* type) {
    if (auto s = Idx::isa_lit(type); s && *s > 2 && *s != 1_n << 8 && *s != 1_n << 16 && *s != 1_n << 32) return s;
    return {};
}

/// The size of the smallest of `I8`, `I16`, `I32`, and `I64` that holds the values of `Idx s`.
nat_t widen(nat_t s) {
    auto w = std::max(8_n, std::bit_ceil(Idx::size2bitwidth(s)));
    return w == 64 ? 0 : 1_n << w;
}

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

const Def* select(const Def* cond, const Def* t, const Def* f) {
    auto& w = cond->world();
    return w.extract(w.tuple({f, t}), cond);
}

} // namespace

const Def* LegalizeIdx::zext(const Def* x) { return new_world().call(conv::u, new_world().lit_nat_0(), x); }

const Def* LegalizeIdx::sext(nat_t s, const Def* x) {
    auto& w  = new_world();
    auto z   = zext(x);
    auto neg = w.call(icmp::ug, Defs{z, w.lit_i64((s - 1) / 2)});
    return select(neg, w.call(wrap::sub, Mode::none, Defs{z, w.lit_i64(s)}), z);
}

const Def* LegalizeIdx::mod_u(nat_t s, const Def* y) { return new_world().call<rem>(new_world().lit_nat(s), y); }

const Def* LegalizeIdx::mod_s(nat_t s, const Def* y) {
    auto& w   = new_world();
    auto zero = w.lit_i64(0);
    auto neg  = w.call(icmp::sl, Defs{y, zero});
    auto m    = mod_u(s, select(neg, w.call(wrap::sub, Mode::none, Defs{zero, y}), y));
    auto flip = select(w.call(icmp::e, Defs{m, zero}), zero, w.call(wrap::sub, Mode::none, Defs{w.lit_i64(s), m}));
    return select(neg, flip, m);
}

const Def* LegalizeIdx::add(nat_t s, const Def* a, const Def* b) {
    auto& w = new_world();
    auto t  = w.call(wrap::add, Mode::none, Defs{a, b});
    // `t < a` catches a sum that wrapped around `I64`.
    auto over = w.call(bit2::or_, 0_n, Defs{w.call(icmp::ul, Defs{t, a}), w.call(icmp::uge, Defs{t, w.lit_i64(s)})});
    return select(over, w.call(wrap::sub, Mode::none, Defs{t, w.lit_i64(s)}), t);
}

const Def* LegalizeIdx::sub(nat_t s, const Def* a, const Def* b) {
    auto& w = new_world();
    auto d  = w.call(wrap::sub, Mode::none, Defs{a, b});
    return select(w.call(icmp::ul, Defs{a, b}), w.call(wrap::add, Mode::none, Defs{d, w.lit_i64(s)}), d);
}

const Def* LegalizeIdx::mul(nat_t s, const Def* a, const Def* b) {
    auto& w = new_world();
    if (s <= nat_t(1) << 32) return mod_u(s, w.call(wrap::mul, Mode::none, Defs{a, b}));

    // The product may exceed `I64`, so double and add bit by bit.
    const Def* r = w.lit_i64(0);
    for (auto i = Idx::size2bitwidth(s); i-- != 0;) {
        auto bit = w.call(bit2::and_, 0_n, Defs{w.call(shr::l, Defs{b, w.lit_i64(i)}), w.lit_i64(1)});
        r        = add(s, r, r);
        r        = add(s, r, select(w.call(icmp::e, Defs{bit, w.lit_i64(0)}), w.lit_i64(0), a));
    }
    return r;
}

const Def* LegalizeIdx::rewrite_imm_App(const App* app) {
    auto& w      = new_world();
    auto narrow  = [&w](nat_t s, const Def* y) { return w.call(conv::u, w.lit_nat(widen(s)), y); };
    auto zext_rw = [this](const Def* d) { return zext(rewrite(d)); };

    if (auto s = odd(app)) return w.type_idx(widen(*s));

    if (auto op = Axm::isa<wrap>(app)) {
        if (auto s = odd(app->type())) {
            auto [a, b] = op->args<2>(zext_rw);
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
        if (auto d = odd(app->type())) {
            if (op.id() == conv::u) {
                if (src && *src != 0 && *src <= *d) return narrow(*d, rewrite(x));
                return narrow(*d, mod_u(*d, zext(rewrite(x))));
            }
            auto y = odd(x->type()) ? sext(*src, rewrite(x)) : w.call(conv::s, w.lit_nat_0(), rewrite(x));
            return narrow(*d, mod_s(*d, y));
        }
        if (auto s = odd(x->type()); s && op.id() == conv::s)
            return w.call(conv::s, rewrite(Idx::isa(app->type())), sext(*s, rewrite(x)));
    } else if (auto op = Axm::isa<bitcast>(app)) {
        if (auto d = odd(app->type())) return narrow(*d, mod_u(*d, w.call<bitcast>(w.type_i64(), rewrite(op->arg()))));
    } else if (auto op = Axm::isa<icmp>(app)) {
        auto [a, b] = op->args<2>();
        if (auto s = odd(a->type())) {
            auto ext = [&](const Def* x) { return is_unsigned(op.id()) ? zext(rewrite(x)) : sext(*s, rewrite(x)); };
            return w.call(op.id(), Defs{ext(a), ext(b)});
        }
    } else if (auto op = Axm::isa<shr>(app)) {
        if (auto s = odd(app->type()); s && op.id() == shr::a) {
            auto [a, b] = op->args<2>();
            return narrow(*s, mod_s(*s, w.call(shr::a, Defs{sext(*s, rewrite(a)), zext(rewrite(b))})));
        }
    } else if (auto op = Axm::isa<extrema>(app)) {
        if (auto s = odd(app->type()); s && (op.id() == extrema::smin || op.id() == extrema::smax)) {
            auto [a, b] = op->args<2>();
            return narrow(*s, mod_s(*s, w.call(op.id(), Defs{sext(*s, rewrite(a)), sext(*s, rewrite(b))})));
        }
    } else if (auto op = Axm::isa<abs>(app)) {
        auto [m, x] = op->args<2>();
        if (auto s = odd(x->type())) {
            auto r = w.call<abs>(Defs{rewrite(m), sext(*s, rewrite(x))});
            return w.tuple({r->proj(2, 0), narrow(*s, mod_s(*s, r->proj(2, 1)))});
        }
    } else if (auto op = Axm::isa<div>(app)) {
        auto [m, xy] = op->args<2>();
        auto [x, y]  = xy->projs<2>();
        if (auto s = odd(x->type()); s && (op.id() == div::sdiv || op.id() == div::srem)) {
            auto r = w.call(op.id(), Defs{rewrite(m), w.tuple({sext(*s, rewrite(x)), sext(*s, rewrite(y))})});
            return w.tuple({r->proj(2, 0), narrow(*s, mod_s(*s, r->proj(2, 1)))});
        }
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LegalizeIdx::index_type(const Def* type) {
    auto& w = new_world();
    if (auto s = odd(type)) return w.type_idx(*s);
    if (auto sigma = type->isa_imm<Sigma>())
        return w.sigma(DefVec(sigma->ops(), [this](const Def* op) { return index_type(op); }));
    if (auto arr = type->isa_imm<Arr>()) return w.arr(rewrite(arr->arity()), index_type(arr->body()));
    return rewrite(type);
}

const Def* LegalizeIdx::rewrite_imm_Extract(const Extract* extract) {
    auto index = coerce(index_type(extract->index()->type()), rewrite(extract->index()));
    return new_world().extract(rewrite(extract->tuple()), index);
}

const Def* LegalizeIdx::rewrite_imm_Insert(const Insert* insert) {
    auto index = coerce(index_type(insert->index()->type()), rewrite(insert->index()));
    return new_world().insert(rewrite(insert->tuple()), index, rewrite(insert->value()));
}

const Def* LegalizeIdx::coerce_leaf(const Def* type, const Def* val) {
    if (auto size = Idx::isa(type); size && Idx::isa(val->type()) && (odd(type) || odd(val->type())))
        return new_world().call(conv::u, size, val);
    return nullptr;
}

} // namespace mim::plug::core::phase
