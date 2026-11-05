#include "mim/plug/core/phase/legalize_idx.h"

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

namespace {

/// @name I64 Arithmetic
///@{
const Def* op64(World& w, wrap id, const Def* a, const Def* b) { return w.call(id, Mode::none, Defs{a, b}); }

/// The signed representative of @p x in `Idx s` as `I64`.
const Def* sext(World& w, nat_t s, const Def* x) {
    auto z   = zext(w, x);
    auto neg = w.call(icmp::ug, Defs{z, w.lit_i64((s - 1) / 2)});
    return w.select(neg, op64(w, wrap::sub, z, w.lit_i64(s)), z);
}

/// The `I64` @p y, which is below @p s, as a widened `Idx s`.
const Def* narrow(World& w, nat_t s, const Def* y) { return w.call(conv::u, w.lit_nat(widen_idx(s)), y); }

const Def* mod_u(World& w, nat_t s, const Def* y) { return w.call<rem>(w.lit_nat(s), y); }

/// The signed `I64` @p y modulo @p s as a widened `Idx s`.
const Def* mod_s(World& w, nat_t s, const Def* y) {
    auto zero = w.lit_i64(0);
    auto neg  = w.call(icmp::sl, Defs{y, zero});
    auto m    = mod_u(w, s, w.select(neg, op64(w, wrap::sub, zero, y), y));
    auto flip = w.select(w.call(icmp::e, Defs{m, zero}), zero, op64(w, wrap::sub, w.lit_i64(s), m));
    return narrow(w, s, w.select(neg, flip, m));
}

/// The `[M, I64]` result @p r of a signed effectful op with its value reduced modulo @p s.
const Def* mod_s_mem(World& w, nat_t s, const Def* r) {
    auto [m, v] = r->projs<2>();
    return w.tuple({m, mod_s(w, s, v)});
}

const Def* add(World& w, nat_t s, const Def* a, const Def* b) {
    auto t    = op64(w, wrap::add, a, b);
    auto over = w.call(icmp::uge, Defs{t, w.lit_i64(s)});
    // Two values below `s` only wrap around `I64` if `s` exceeds `2^63`; `t < a` catches that.
    if (s > 1_n << 63) over = w.call(bit2::or_, 0_n, Defs{w.call(icmp::ul, Defs{t, a}), over});
    return narrow(w, s, w.select(over, op64(w, wrap::sub, t, w.lit_i64(s)), t));
}

const Def* sub(World& w, nat_t s, const Def* a, const Def* b) {
    auto d = op64(w, wrap::sub, a, b);
    return narrow(w, s, w.select(w.call(icmp::ul, Defs{a, b}), op64(w, wrap::add, d, w.lit_i64(s)), d));
}

/// The product of the widened `Idx s` values @p a and @p b.
const Def* mul(World& w, nat_t s, const Def* a, const Def* b) { return w.call<mulrem>(w.lit_nat(s), Defs{a, b}); }

/// The bits of @p x as `I64`, zero-extended if @p x is narrower.
const Def* bits(World& w, const Def* x) {
    auto type = x->type();
    if (!Idx::isa(type) && !type->isa<Nat>())
        if (auto size = Lit::isa(op(trait::size, type)); size && *size < 8)
            return zext(w, w.call<bitcast>(w.type_int(8 * *size), x));
    return w.call<bitcast>(w.type_i64(), x);
}
///@}

} // namespace

const Def* LegalizeIdx::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    auto zx = [&](const Def* d) { return zext(w, rewrite(d)); };
    auto sx = [&](nat_t s, const Def* d) { return sext(w, s, rewrite(d)); };

    if (auto s = isa_odd_idx(app)) return w.type_idx(widen_idx(*s));

    if (auto op = Axm::isa<conv>(app); op && op.id() == conv::u) {
        auto x = op->arg();
        if (auto d = isa_odd_idx(app->type())) {
            if (auto src = Idx::isa_lit(x->type()); src && *src != 0 && *src <= *d) return narrow(w, *d, rewrite(x));
            return narrow(w, *d, mod_u(w, *d, zx(x)));
        }
    }

    auto s = isa_odd_idx_op(app);
    if (!s) {
        // An op that keeps `Idx s` yields its value in range but still has to widen it like everything else.
        auto res = RWPhase::rewrite_imm_App(app);
        if (auto type = rewrite(app->type()); type != res->type() && !type->isa<Pi>()) return coerce(type, res);
        return res;
    }

    if (auto op = Axm::isa<wrap>(app)) {
        auto [a, b] = op->args<2>([this](const Def* d) { return rewrite(d); });
        auto pow    = [&] { return narrow(w, *s, mod_u(w, *s, op64(w, wrap::shl, w.lit_i64(1), zext(w, b)))); };
        switch (op.id()) {
            case wrap::add: return add(w, *s, zext(w, a), zext(w, b));
            case wrap::sub: return sub(w, *s, zext(w, a), zext(w, b));
            case wrap::mul: return mul(w, *s, a, b);
            case wrap::shl: return mul(w, *s, a, pow());
        }
    } else if (Axm::isa<bitcast>(app) || Axm::isa<idx>(app)) {
        return narrow(w, *s, mod_u(w, *s, bits(w, rewrite(app->arg()))));
    } else if (auto op = Axm::isa<conv>(app)) {
        auto x = op->arg();
        if (auto d = isa_odd_idx(app->type())) {
            auto src = isa_odd_idx(x->type());
            return mod_s(w, *d, src ? sx(*src, x) : w.call(conv::s, w.lit_nat_0(), rewrite(x)));
        }
        return w.call(conv::s, rewrite(Idx::isa(app->type())), sx(*s, x));
    } else if (auto op = Axm::isa<icmp>(app)) {
        auto [a, b] = op->args<2>([&](const Def* x) { return sx(*s, x); });
        return w.call(op.id(), Defs{a, b});
    } else if (auto op = Axm::isa<shr>(app)) {
        auto [a, b] = op->args<2>();
        return mod_s(w, *s, w.call(shr::a, Defs{sx(*s, a), zx(b)}));
    } else if (auto op = Axm::isa<extrema>(app)) {
        auto [a, b] = op->args<2>();
        return mod_s(w, *s, w.call(op.id(), Defs{sx(*s, a), sx(*s, b)}));
    } else if (auto op = Axm::isa<abs>(app)) {
        auto [m, x] = op->args<2>();
        return mod_s_mem(w, *s, w.call<abs>(Defs{rewrite(m), sx(*s, x)}));
    } else if (auto op = Axm::isa<div>(app)) {
        auto [m, xy] = op->args<2>();
        auto [x, y]  = xy->projs<2>();
        return mod_s_mem(w, *s, w.call(op.id(), Defs{rewrite(m), w.tuple({sx(*s, x), sx(*s, y)})}));
    }
    std::unreachable();
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
