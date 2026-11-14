#include "mim/plug/x86/phase/isel.h"

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

/// Width of the register holding an `Idx size`.
nat_t reg_width(nat_t size) { return std::bit_ceil(std::max<nat_t>(Idx::size2bitwidth(size), 8)); }

bool is_reg_size(nat_t size) { return size == 2 || size == Idx::bitwidth2size(reg_width(size)); }

nat_t trait(core::trait t, const Def* type) {
    if (auto n = Lit::isa(core::op(t, type))) return *n;
    type->blame(MIM_X86_BE "type `{}` has no static size or alignment", type).bail();
}

std::optional<nat_t> cc(core::icmp id) {
    switch (id) {
        case core::icmp::e: return 4;
        case core::icmp::ne: return 5;
        case core::icmp::ul: return 2;
        case core::icmp::uge: return 3;
        case core::icmp::ule: return 6;
        case core::icmp::ug: return 7;
        case core::icmp::sl: return 12;
        case core::icmp::sge: return 13;
        case core::icmp::sle: return 14;
        case core::icmp::sg: return 15;
        default: return {};
    }
}

std::optional<nat_t> cc(core::ncmp id) {
    switch (id) {
        case core::ncmp::e: return 4;
        case core::ncmp::ne: return 5;
        case core::ncmp::l: return 2;
        case core::ncmp::ge: return 3;
        case core::ncmp::le: return 6;
        case core::ncmp::g: return 7;
        default: return {};
    }
}

template<class Id>
const Def* bin(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{a, b});
}

/// All bits of @p type set.
const Def* ones(World& w, const Def* type) {
    auto s = *Idx::isa_lit(type);
    return w.lit(type, s == 0 ? ~0_n : s - 1);
}

bool is_lowered_plugin(const Axm* axm) {
    auto p = axm->plugin();
    return p == Annex::flags2plugin(Annex::Base<core::M>) || p == Annex::flags2plugin(Annex::Base<mem::load>)
        || p == Annex::flags2plugin(Annex::Base<math::F>);
}

} // namespace

const Def* ISel::zext(const Def* d, const Def* x) {
    if (auto l = Lit::isa(x)) return new_world().lit(new_world().type_idx(d), *l);
    return new_world().call<movzx>(d, x);
}

const Def* ISel::val(const Def* def) {
    auto res = rewrite(def);
    if (auto l = Lit::isa(res); l && res->type()->isa<Nat>()) return new_world().lit_i64(*l);
    return res;
}

const Def*
ISel::wide(const Def* (*insn)(World&, const Def*, const Def*), const Def* type, bool sign, const Def* a, const Def* b) {
    auto& w = new_world();
    auto s  = Idx::isa_lit(type);
    if (!s || reg_width(*s) >= 16) return insn(w, a, b);
    auto ext = [&](const Def* x) { return sign ? w.call<movsx>(w.lit_i32(), x) : zext(w.lit_i32(), x); };
    return w.call<trunc>(*s, insn(w, ext(a), ext(b)));
}

const Def* ISel::rewrite_imm_Lit(const Lit* lit) {
    if (lit->type()->isa<Nat>()) return new_world().lit_nat(lit->get());
    return RWPhase::rewrite_imm_Lit(lit);
}

const Def* ISel::coerce_leaf(const Def* type, const Def* val) {
    if (auto l = Lit::isa(val); l && val->type()->isa<Nat>() && type == new_world().type_i64())
        return new_world().lit_i64(*l);
    return nullptr;
}

const Def* ISel::rewrite_imm_Extract(const Extract* ex) {
    auto& w    = new_world();
    auto type  = rewrite(ex->type());
    auto s     = Idx::isa_lit(type);
    auto index = ex->index();
    if (!s || Idx::isa_lit(index->type()) != 2 || Lit::isa(index) || ex->tuple()->num_projs() != 2)
        return RWPhase::rewrite_imm_Extract(ex);

    auto [x, y] = ex->tuple()->projs<2>([this](const Def* d) { return val(d); });
    auto c      = rewrite(index);
    auto code   = 5_n;
    auto fl     = w.call<test>(Defs{c, c});
    if (auto app = c->isa<App>(); app && app->axm() && app->axm()->flags() == Annex::Base<x86::cc>)
        code = *Lit::isa(app->callee()->as<App>()->arg()), fl = app->arg();
    // `cmov` has no 8-bit form.
    if (reg_width(*s) < 16)
        return w.call<trunc>(*s, w.call<cmovcc>(code, Defs{fl, zext(w.lit_i32(), x), zext(w.lit_i32(), y)}));
    return w.call<cmovcc>(code, Defs{fl, x, y});
}

const Def* ISel::offset(const Def* pointee, const Def* elem, const Def* index) {
    auto& w = new_world();
    if (pointee->isa<Arr>()) {
        auto size = trait(core::trait::size, elem);
        if (auto i = Lit::isa(index)) return w.lit_i64(*i * size);

        auto i = rewrite(index);
        if (auto n = Idx::isa_lit(index->type()); n && *n != 0) i = zext(w.lit_i64(), i);
        return size == 1 ? i : w.call<imul>(Defs{i, w.lit_i64(size)});
    }

    auto i = Lit::isa(index);
    if (!i) index->blame(MIM_X86_BE "index `{}` into `{}` must be a literal", index, pointee).bail();
    nat_t res = 0;
    for (nat_t j = 0; j <= *i; ++j) {
        auto field = pointee->proj(pointee->num_projs(), j);
        auto align = trait(core::trait::align, field);
        res        = (res + align - 1) / align * align;
        if (j != *i) res += trait(core::trait::size, field);
    }
    return w.lit_i64(res);
}

const Def* ISel::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    auto rw = [this](const Def* d) { return rewrite(d); };

    if (Axm::isa<core::Ptr>(app)) return w.type_i64();
    if (Axm::isa<core::M>(app)) return w.call<core::M>(w.lit_nat_0());
    if (auto size = Idx::isa(app); size && !Lit::isa(size)) return w.type_i64();

    if (auto op = Axm::isa<core::wrap>(app)) {
        auto [a, b] = op->args<2>(rw);
        switch (op.id()) {
            case core::wrap::add: return w.call<add>(Defs{a, b});
            case core::wrap::sub: return w.call<sub>(Defs{a, b});
            // The low bits of a product do not depend on how the operands were extended.
            case core::wrap::mul: return wide(bin<imul>, app->type(), false, a, b);
            case core::wrap::shl: return w.call<shl>(Defs{a, b});
        }
    } else if (auto op = Axm::isa<core::bit2>(app)) {
        auto [a, b] = op->args<2>(rw);
        switch (op.id()) {
            case core::bit2::and_: return w.call<and_>(Defs{a, b});
            case core::bit2::or_: return w.call<or_>(Defs{a, b});
            case core::bit2::xor_: return w.call<xor_>(Defs{a, b});
            case core::bit2::nand: return w.call<xor_>(Defs{w.call<and_>(Defs{a, b}), ones(w, a->type())});
            case core::bit2::nor: return w.call<xor_>(Defs{w.call<or_>(Defs{a, b}), ones(w, a->type())});
            case core::bit2::nxor: return w.call<xor_>(Defs{w.call<xor_>(Defs{a, b}), ones(w, a->type())});
            default: break;
        }
    } else if (auto op = Axm::isa<core::shr>(app)) {
        auto [a, b] = op->args<2>(rw);
        return op.id() == core::shr::a ? w.call<sar>(Defs{a, b}) : w.call<shr>(Defs{a, b});
    } else if (auto op = Axm::isa<core::icmp>(app)) {
        if (auto c = cc(op.id())) {
            auto [a, b] = op->args<2>(rw);
            return w.call<x86::cc>(*c, w.call<cmp>(Defs{a, b}));
        }
    } else if (auto op = Axm::isa<core::conv>(app)) {
        auto src = Idx::isa_lit(rewrite(op->arg()->type()));
        auto dst = Idx::isa_lit(rewrite(app->type()));
        if (src && dst) {
            auto x = rewrite(op->arg());
            if (*src == *dst) return x;
            auto sw = reg_width(*src);
            auto dw = reg_width(*dst);
            if (sw >= dw) return w.call<trunc>(*dst, x);
            return op.id() == core::conv::s ? w.call<movsx>(*dst, x) : w.call<movzx>(*dst, x);
        }
    } else if (auto op = Axm::isa<core::idx>(app)) {
        if (rewrite(app->type()) == w.type_i64()) return val(op->arg());
    } else if (auto op = Axm::isa<core::idx2nat>(app)) {
        auto x = rewrite(op->arg());
        return Idx::isa_lit(x->type()) == 0 ? x : zext(w.lit_i64(), x);
    } else if (auto op = Axm::isa<core::bitcast>(app)) {
        auto x   = rewrite(op->arg());
        auto src = Idx::isa_lit(x->type());
        auto dst = Idx::isa_lit(rewrite(app->type()));
        if (x->type() == rewrite(app->type())) return x;
        if (src && dst) return reg_width(*src) >= reg_width(*dst) ? w.call<trunc>(*dst, x) : w.call<movzx>(*dst, x);
    } else if (auto op = Axm::isa<core::nat>(app)) {
        auto [a, b] = op->args<2>([this](const Def* d) { return val(d); });
        switch (op.id()) {
            case core::nat::add: return w.call<add>(Defs{a, b});
            case core::nat::mul: return w.call<imul>(Defs{a, b});
            case core::nat::sub:
                return w.call<cmovcc>(2, Defs{w.call<cmp>(Defs{a, b}), w.call<sub>(Defs{a, b}), w.lit_i64(0)});
            case core::nat::div: return w.call<div>(Defs{a, b});
            case core::nat::rem: return w.call<rem>(Defs{a, b});
        }
    } else if (auto op = Axm::isa<core::ncmp>(app)) {
        if (auto c = cc(op.id())) {
            auto [a, b] = op->args<2>([this](const Def* d) { return val(d); });
            return w.call<x86::cc>(*c, w.call<cmp>(Defs{a, b}));
        }
    } else if (auto op = Axm::isa<core::rem>(app)) {
        auto x = rewrite(op->arg());
        auto d = w.lit(x->type(), *Lit::isa(op->decurry()->arg()));
        return wide(bin<rem>, x->type(), false, x, d);
    } else if (auto op = Axm::isa<core::div>(app)) {
        auto [m, xy] = op->args<2>();
        auto [x, y]  = xy->projs<2>(rw);
        auto type    = x->type();
        const Def* res;
        switch (op.id()) {
            case core::div::udiv: res = wide(bin<div>, type, false, x, y); break;
            case core::div::urem: res = wide(bin<rem>, type, false, x, y); break;
            case core::div::sdiv: res = wide(bin<idiv>, type, true, x, y); break;
            case core::div::srem: res = wide(bin<irem>, type, true, x, y); break;
        }
        return w.tuple({rewrite(m), res});
    } else if (auto op = Axm::isa<mem::mslot>(app)) {
        auto [T, _]    = op->decurry()->args<2>();
        auto [msz, k]  = op->args<2>();
        auto [m, size] = msz->projs<2>();
        auto n         = Lit::isa(size).value_or(trait(core::trait::size, T));
        return w.app(rewrite(k), w.call<slot>(n, rewrite(m)));
    } else if (auto op = Axm::isa<mem::assume>(app)) {
        return rewrite(op->arg(2, 0));
    } else if (auto op = Axm::isa<mem::load>(app)) {
        auto [_, T] = app->type()->projs<2>();
        if (auto s = Idx::isa_lit(rewrite(T)); s && is_reg_size(*s)) {
            auto [m, p] = op->args<2>(rw);
            return w.call<load>(*s, Defs{m, p});
        }
    } else if (auto op = Axm::isa<mem::store>(app)) {
        if (auto s = Idx::isa_lit(rewrite(op->arg(3, 2)->type())); s && is_reg_size(*s)) {
            auto [m, p, v] = op->args<3>(rw);
            return w.call<store>(Defs{m, p, v});
        }
    } else if (auto op = Axm::isa<mem::lea>(app)) {
        auto [p, i]       = op->args<2>();
        auto [pointee, _] = Axm::as<core::Ptr>(p->type())->args<2>();
        auto [elem, _2]   = Axm::as<core::Ptr>(app->type())->args<2>();
        return w.call<add>(Defs{rewrite(p), offset(pointee, elem, i)});
    }

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !Axm::isa<core::M>(app)
                               && !app->type()->isa<Type>() && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
