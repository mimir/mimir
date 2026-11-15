#include "mim/plug/x86/phase/isel.h"

#include <pbqp/solver.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

/// Width of the register holding an `Idx size`.
nat_t reg_width(nat_t size) { return std::bit_ceil(std::max<nat_t>(Idx::size2bitwidth(size), 8)); }

bool fits_i32(s64 disp) { return INT32_MIN <= disp && disp <= INT32_MAX; }

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

/// Does the old_world() or new_world() literal @p lit fit a sign-extended 32-bit immediate?
bool fits_imm(const Def* lit) {
    auto s = Idx::isa_lit(lit->type());
    return (s && reg_width(*s) < 64) || fits_i32(*Lit::isa(lit));
}

/// @p x in a register.
const Def* reg(World& w, const Def* x) { return Lit::isa(x) ? w.call<mov>(x) : x; }
/// @p x in a register or as an immediate.
const Def* ri(World& w, const Def* x) { return Lit::isa(x) && fits_imm(x) ? x : reg(w, x); }

/// `op r, r/imm`.
template<class Id>
const Def* bin(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{reg(w, a), ri(w, b)});
}

/// `op r, r`.
template<class Id>
const Def* rr(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{reg(w, a), reg(w, b)});
}

/// Result @p I of `div r` or `idiv r`: quotient or remainder.
template<class Id, size_t I>
const Def* divrem(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{reg(w, a), reg(w, b)})->proj(2, I);
}

/// A shift by `cl` or an immediate.
template<class Id>
const Def* shift(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{reg(w, a), b});
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

/// The x86 instruction for the `core` operation @p def if it has a memory-operand form (read-modify-write if @p rmw).
std::optional<flags_t> alu_id(const Def* def, bool rmw) {
    auto s = Idx::isa_lit(def->type());
    if (!s || !is_reg_size(*s)) return {};
    if (auto op = Axm::isa<core::wrap>(def)) {
        switch (op.id()) {
            case core::wrap::add: return Annex::Base<add>;
            case core::wrap::sub: return Annex::Base<sub>;
            // `imul` has neither an 8-bit nor a read-modify-write form.
            case core::wrap::mul: return !rmw && reg_width(*s) >= 16 ? std::optional(Annex::Base<imul>) : std::nullopt;
            default: return {};
        }
    } else if (auto op = Axm::isa<core::bit2>(def)) {
        switch (op.id()) {
            case core::bit2::and_: return Annex::Base<and_>;
            case core::bit2::or_: return Annex::Base<or_>;
            case core::bit2::xor_: return Annex::Base<xor_>;
            default: return {};
        }
    }
    return {};
}

bool commutes(const Def* def) {
    auto op = Axm::isa<core::wrap>(def);
    return !op || op.id() != core::wrap::sub;
}

/// The continuation whose `mem.M` chain @p m is on.
Lam* chain(const Def* m) {
    for (;;) {
        if (auto ex = m->isa<Extract>()) m = ex->tuple();
        if (auto var = m->isa<Var>()) return var->binder()->isa<Lam>();
        auto app = m->isa<App>();
        if (!app) return nullptr;
        auto arg  = app->arg();
        auto n    = arg->num_projs();
        auto next = (const Def*)nullptr;
        for (size_t i = 0; i != n && !next; ++i)
            if (auto a = arg->proj(n, i); Axm::isa<core::M>(a->type())) next = a;
        if (!next) return nullptr;
        m = next;
    }
}

/// Does @p def depend on @p target without going through a mutable?
bool depends(const Def* def, const Def* target, DefSet& done) {
    if (def == target) return true;
    if (def->isa_mut() || !done.emplace(def).second) return false;
    for (auto op : def->ops())
        if (depends(op, target, done)) return true;
    return false;
}

/// The size of the x86 `Idx` the old_world() @p type becomes, if it fits a register.
std::optional<nat_t> xsize(const Def* type) {
    if (Axm::isa<core::Ptr>(type) || type->isa<Nat>()) return 0;
    if (auto size = Idx::isa(type)) {
        auto s = Lit::isa(size).value_or(0);
        return is_reg_size(s) ? std::optional(s) : std::nullopt;
    }
    return {};
}

/// Size of the elements the array `mem.lea` @p lea indexes.
nat_t elem_size(const App* lea) {
    auto [elem, _] = Axm::as<core::Ptr>(lea->type())->args<2>();
    return trait(core::trait::size, elem);
}

/// The constant byte offset of the `mem.lea` @p lea, unless its index is dynamic.
std::optional<nat_t> offset(const App* lea) {
    auto [p, i]       = Axm::as<mem::lea>(lea)->args<2>();
    auto [pointee, _] = Axm::as<core::Ptr>(p->type())->args<2>();
    auto l            = Lit::isa(i);
    if (pointee->isa<Arr>()) return l ? std::optional(*l * elem_size(lea)) : std::nullopt;
    if (!l) i->blame(MIM_X86_BE "index `{}` into `{}` must be a literal", i, pointee).bail();
    nat_t off = 0;
    for (nat_t j = 0; j <= *l; ++j) {
        auto field = pointee->proj(pointee->num_projs(), j);
        auto align = trait(core::trait::align, field);
        off        = (off + align - 1) / align * align;
        if (j != *l) off += trait(core::trait::size, field);
    }
    return off;
}

/// The x86 ALU instruction for the old_world() operation @p def; it has a memory operand.
const Def* alu(World& w, const Def* def) {
    return w.app(w.annex(*alu_id(def, false)), w.lit_nat(*Idx::isa_lit(def->type())));
}

/// Applies @p insn, going through `i32` for operands narrower than 16 bits.
const Def* wide(ISel& s,
                const Def* (*insn)(World&, const Def*, const Def*),
                const Def* type,
                bool sign,
                const Def* a,
                const Def* b) {
    auto& w   = s.new_world();
    auto size = Idx::isa_lit(type);
    if (!size || reg_width(*size) >= 16) return insn(w, a, b);
    auto ext = [&](const Def* x) { return sign ? w.call<movsx>(w.lit_i32(), reg(w, x)) : s.zext(w.lit_i32(), x); };
    return w.call<trunc>(*size, insn(w, ext(a), ext(b)));
}

/*
 * Costs
 */

/// Cost of converting a replacement of type @p from into a hole of type @p want.
pbqp::Cost conv(Ty from, Want want) {
    switch (want) {
        case Want::Reg: return from == Ty::Reg ? 0 : 1;
        case Want::Imm: return pbqp::Cost::inf();
        case Want::Base: return from == Ty::Index ? 1 : 0;
        case Want::Index: return from == Ty::Index ? 0 : pbqp::Cost::inf();
        case Want::Addr: return 0;
    }
    std::unreachable();
}

/// Cost of the operand @p def, which is no node, in a hole of type @p want: a literal that is no immediate and not
/// part of an address needs a `mov`.
pbqp::Cost leaf(const Def* def, Want want) {
    if (!Lit::isa(def)) return conv(Ty::Reg, want);
    return want == Want::Reg ? 1 : want == Want::Index ? pbqp::Cost::inf() : 0;
}

/*
 * Rules
 */

constexpr u64 Load = 1; ///< The inner node `mem.load (m, p: Addr i)`.

Alt root(const Rule& rule,
         pbqp::Cost cost,
         std::initializer_list<Demand> demands,
         std::array<const Def*, 3> bind = {}) {
    auto alt    = Alt();
    alt.rule    = &rule;
    alt.cost    = cost;
    alt.demands = demands;
    alt.bind    = bind;
    return alt;
}

Want ri(const Def* def) { return Lit::isa(def) && fits_imm(def) ? Want::Imm : Want::Reg; }

const Def* build_lea_off(ISel& s, const Def* root, const Alt& alt) {
    auto a = s.split(s.hole(alt.demands[0].def, alt.demands[0].want));
    a.disp += *offset(root->as<App>());
    return s.addr(a);
}

const Def* build_lea_idx(ISel& s, const Def* root, const Alt& alt) {
    auto& w   = s.new_world();
    auto i    = alt.demands[1].def;
    auto a    = s.split(s.hole(alt.demands[0].def, Want::Base));
    auto size = elem_size(root->as<App>());
    auto x    = s.hole(i);
    if (auto n = Idx::isa_lit(i->type()); n && *n != 0) x = s.zext(w.lit_i64(), x);
    if (size == 1 || size == 2 || size == 4 || size == 8) return s.addr({a.base, x, u8(size), a.disp});
    return s.addr({a.base, bin<imul>(w, x, w.lit_i64(size)), 1, a.disp});
}

const Def* build_load(ISel& s, const Def* root, const Alt&) {
    auto [m, p] = root->as<App>()->args<2>();
    auto size   = *xsize(root->type()->proj(2, 1));
    return s.new_world().call<load>(size, Defs{s.rewrite(m), s.hole(p, Want::Addr)});
}

const Def* build_store(ISel& s, const Def* root, const Alt& alt) {
    auto [m, p, x] = root->as<App>()->args<3>();
    return s.new_world().call<store>(Defs{s.rewrite(m), s.hole(p, Want::Addr), s.hole(x, alt.demands[1].want)});
}

const Def* build_store_mr(ISel& s, const Def* root, const Alt& alt) {
    auto& w             = s.new_world();
    auto [_, p, u]      = root->as<App>()->args<3>();
    auto [ld, other, x] = alt.bind;
    auto b              = s.hole(other, alt.covers[0].demands[1].want);
    return w.call<mr>(alu(w, u), Defs{s.rewrite(ld->as<App>()->arg(2, 0)), s.hole(p, Want::Addr), b});
}

const Def* build_bin(ISel& s, const Def* root, const Alt& alt) {
    auto& w  = s.new_world();
    auto app = root->as<App>();
    auto a   = s.hole(alt.bind[0], alt.demands[0].want);
    auto b   = s.hole(alt.bind[1], alt.demands[1].want);
    if (auto op = Axm::isa<core::wrap>(app)) {
        switch (op.id()) {
            case core::wrap::add: return bin<add>(w, a, b);
            case core::wrap::sub: return bin<sub>(w, a, b);
            // The low bits of a product do not depend on how the operands were extended.
            case core::wrap::mul: return wide(s, bin<imul>, app->type(), false, a, b);
            case core::wrap::shl: return shift<shl>(w, a, b);
        }
    } else if (auto op = Axm::isa<core::bit2>(app)) {
        switch (op.id()) {
            case core::bit2::and_: return bin<and_>(w, a, b);
            case core::bit2::or_: return bin<or_>(w, a, b);
            case core::bit2::xor_: return bin<xor_>(w, a, b);
            case core::bit2::nand: return bin<xor_>(w, bin<and_>(w, a, b), ones(w, a->type()));
            case core::bit2::nor: return bin<xor_>(w, bin<or_>(w, a, b), ones(w, a->type()));
            case core::bit2::nxor: return bin<xor_>(w, bin<xor_>(w, a, b), ones(w, a->type()));
            default: return nullptr;
        }
    }
    auto op = Axm::as<core::shr>(app);
    return op.id() == core::shr::a ? shift<sar>(w, a, b) : shift<shr>(w, a, b);
}

const Def* build_bin_rm(ISel& s, const Def* root, const Alt& alt) {
    auto& w     = s.new_world();
    auto ld     = alt.bind[0]->as<App>();
    auto [m, p] = ld->args<2>();
    auto res    = w.call<rm>(alu(w, root), Defs{s.rewrite(m), s.hole(alt.bind[1]), s.hole(p, Want::Addr)});
    s.map(ld, res);
    return res->proj(2, 1);
}

const Def* build_cmp(ISel& s, const Def* root, const Alt& alt) {
    auto& w   = s.new_world();
    auto a    = s.hole(alt.bind[0]);
    auto b    = s.hole(alt.bind[1], alt.demands[1].want);
    auto op   = Axm::isa<core::icmp>(root);
    auto code = op ? cc(op.id()) : cc(Axm::as<core::ncmp>(root).id());
    return w.call<x86::cc>(*code, bin<cmp>(w, a, b));
}

const Def* build_nat(ISel& s, const Def* root, const Alt& alt) {
    auto& w = s.new_world();
    auto a  = s.hole(alt.bind[0]);
    auto b  = s.hole(alt.bind[1], alt.demands[1].want);
    switch (Axm::as<core::nat>(root).id()) {
        case core::nat::add: return bin<add>(w, a, b);
        case core::nat::mul: return bin<imul>(w, a, b);
        case core::nat::sub: return w.call<cmovcc>(2, Defs{bin<cmp>(w, a, b), bin<sub>(w, a, b), reg(w, w.lit_i64(0))});
        case core::nat::div: return divrem<div, 0>(w, a, b);
        case core::nat::rem: return divrem<div, 1>(w, a, b);
    }
    std::unreachable();
}

const Def* build_unary(ISel& s, const Def* root, const Alt& alt) {
    auto& w  = s.new_world();
    auto app = root->as<App>();
    auto x   = s.hole(alt.demands[0].def);
    if (auto op = Axm::isa<core::conv>(app)) {
        auto src = Idx::isa_lit(x->type());
        auto dst = Idx::isa_lit(s.rewrite(app->type()));
        if (!src || !dst) return nullptr;
        if (*src == *dst) return x;
        if (reg_width(*src) >= reg_width(*dst)) return w.call<trunc>(*dst, x);
        return op.id() == core::conv::s ? w.call<movsx>(*dst, x) : w.call<movzx>(*dst, x);
    } else if (Axm::isa<core::idx>(app)) {
        return s.rewrite(app->type()) == w.type_i64() ? x : nullptr;
    } else if (Axm::isa<core::idx2nat>(app)) {
        return Idx::isa_lit(x->type()) == 0 ? x : s.zext(w.lit_i64(), x);
    } else if (Axm::isa<core::bitcast>(app)) {
        auto src = Idx::isa_lit(x->type());
        auto dst = Idx::isa_lit(s.rewrite(app->type()));
        if (x->type() == s.rewrite(app->type())) return x;
        if (!src || !dst) return nullptr;
        return reg_width(*src) >= reg_width(*dst) ? w.call<trunc>(*dst, x) : w.call<movzx>(*dst, x);
    }
    auto op = Axm::as<core::rem>(app);
    auto d  = w.lit(x->type(), *Lit::isa(op->decurry()->arg()));
    return wide(s, divrem<div, 1>, x->type(), false, x, d);
}

const Def* build_div(ISel& s, const Def* root, const Alt& alt) {
    auto& w   = s.new_world();
    auto op   = Axm::as<core::div>(root);
    auto x    = s.hole(alt.bind[0]);
    auto y    = s.hole(alt.bind[1]);
    auto type = x->type();
    const Def* res;
    switch (op.id()) {
        case core::div::udiv: res = wide(s, divrem<div, 0>, type, false, x, y); break;
        case core::div::urem: res = wide(s, divrem<div, 1>, type, false, x, y); break;
        case core::div::sdiv: res = wide(s, divrem<idiv, 0>, type, true, x, y); break;
        case core::div::srem: res = wide(s, divrem<idiv, 1>, type, true, x, y); break;
        default: std::unreachable();
    }
    return w.tuple({s.rewrite(op->arg(2, 0)), res});
}

const Def* build_slot(ISel& s, const Def* root, const Alt&) {
    auto& w        = s.new_world();
    auto op        = Axm::as<mem::mslot>(root);
    auto [T, _]    = op->decurry()->args<2>();
    auto [msz, k]  = op->args<2>();
    auto [m, size] = msz->projs<2>();
    auto n         = Lit::isa(size).value_or(trait(core::trait::size, T));
    return w.app(s.rewrite(k), w.call<slot>(n, s.rewrite(m)));
}

const Def* build_assume(ISel& s, const Def* root, const Alt&) { return s.rewrite(root->as<App>()->arg(2, 0)); }

const Def* build_select(ISel& s, const Def* root, const Alt& alt) {
    auto& w   = s.new_world();
    auto size = *xsize(root->type());
    auto x    = s.hole(alt.bind[0]);
    auto y    = s.hole(alt.bind[1]);
    auto c    = s.hole(alt.bind[2]);
    auto code = 5_n;
    auto fl   = rr<test>(w, c, c);
    if (auto app = c->isa<App>(); app && app->axm() && app->axm()->flags() == Annex::Base<x86::cc>)
        code = *Lit::isa(app->callee()->as<App>()->arg()), fl = app->arg();
    // `cmov` has no 8-bit form.
    if (reg_width(size) < 16) {
        auto ext = [&](const Def* v) { return reg(w, s.zext(w.lit_i32(), v)); };
        return w.call<trunc>(size, w.call<cmovcc>(code, Defs{fl, ext(x), ext(y)}));
    }
    return w.call<cmovcc>(code, Defs{fl, x, y});
}

// clang-format off
constexpr Rule lea_off_base{"lea_off_base", Ty::Base, build_lea_off};
constexpr Rule lea_off_index{"lea_off_index", Ty::Index, build_lea_off};
constexpr Rule lea_index {"lea_index",  Ty::Index, build_lea_idx};
constexpr Rule load_   {"load",     Ty::Reg,   build_load};
constexpr Rule store_  {"store",    Ty::Reg,   build_store};
constexpr Rule store_mr{"store_mr", Ty::Reg,   build_store_mr};
constexpr Rule bin_    {"bin",      Ty::Reg,   build_bin};
constexpr Rule bin_rm  {"bin_rm",   Ty::Reg,   build_bin_rm};
constexpr Rule cmp_    {"cmp",      Ty::Reg,   build_cmp};
constexpr Rule nat_    {"nat",      Ty::Reg,   build_nat};
constexpr Rule unary   {"unary",    Ty::Reg,   build_unary};
constexpr Rule div_    {"div",      Ty::Reg,   build_div};
constexpr Rule slot_   {"slot",     Ty::Reg,   build_slot};
constexpr Rule assume_ {"assume",   Ty::Reg,   build_assume};
constexpr Rule select_ {"select",   Ty::Reg,   build_select};
// clang-format on

/// Appends the matches rooted at @p def to @p alts, an atomic one first.
void match(const ISel& s, const Def* def, Alts& alts) {
    if (auto ex = def->isa<Extract>()) {
        auto index = ex->index();
        if (xsize(ex->type()) && Idx::isa_lit(index->type()) == 2 && !Lit::isa(index)
            && ex->tuple()->num_projs() == 2) {
            auto [x, y] = ex->tuple()->projs<2>();
            alts.emplace_back(root(select_, 1, {{x}, {y}, {index}}, {x, y, index}));
        }
        return;
    }

    auto app = def->isa<App>();
    if (!app) return;

    if (auto lea = Axm::isa<mem::lea>(app)) {
        auto [p, i] = lea->args<2>();
        if (offset(app)) {
            alts.emplace_back(root(lea_off_base, 0, {{p, Want::Base}}));
            alts.emplace_back(root(lea_off_index, 0, {{p, Want::Index}}));
        } else {
            auto size = elem_size(app);
            auto cost = size == 1 || size == 2 || size == 4 || size == 8 ? 0 : 1;
            alts.emplace_back(root(lea_index, cost, {{p, Want::Base}, {i}}));
        }
    } else if (auto ld = Axm::isa<mem::load>(app)) {
        if (xsize(app->type()->proj(2, 1))) alts.emplace_back(root(load_, 1, {{ld->arg(2, 1), Want::Addr}}));
    } else if (auto st = Axm::isa<mem::store>(app)) {
        auto [m, p, x] = st->args<3>();
        if (!xsize(x->type())) return;
        alts.emplace_back(root(store_, 1, {{p, Want::Addr}, {x}}));
        if (ri(x) == Want::Imm) alts.emplace_back(root(store_, 1, {{p, Want::Addr}, {x, Want::Imm}}));
        if (auto u = x->isa<App>(); u && s.uses(u) == 1 && alu_id(u, true) && s.uses(u->arg()) == 1) {
            auto [a, b] = u->args<2>();
            for (auto [v, other] : {std::pair{a, b}, std::pair{b, a}}) {
                if (v == b && !commutes(u)) break;
                // The load's memory has no other user, so nothing lies between load and store.
                if (auto ld = s.sole_load(v);
                    ld && v != other && ld->arg(2, 1) == p && ld->proj(2, 0) == m && s.uses(m) == 1) {
                    auto inner = (*alu_id(u, true) << 2) | (v == b ? 2 : 0) | (ri(other) == Want::Imm ? 1 : 0);
                    auto alt   = root(store_mr, 1, {{p, Want::Addr}, {x, Want::Reg, inner}}, {ld, other, u});
                    alt.covers.emplace_back(Cover{u, inner, {{v, Want::Reg, Load}, {other, ri(other)}}});
                    alt.covers.emplace_back(Cover{ld, Load, {{p, Want::Addr}}});
                    alts.emplace_back(std::move(alt));
                }
            }
        }
    } else if (Axm::isa<core::wrap>(app) || Axm::isa<core::bit2>(app) || Axm::isa<core::shr>(app)) {
        auto w      = Axm::isa<core::wrap>(app);
        auto b2     = Axm::isa<core::bit2>(app);
        auto [a, b] = app->args<2>();
        auto sh     = !b2 && (!w || w.id() == core::wrap::shl);
        alts.emplace_back(root(bin_, 1, {{a}, {b}}, {a, b}));
        if (Lit::isa(b) && (sh || fits_imm(b)))
            alts.emplace_back(root(bin_, 1, {{a}, {b, Want::Imm}}, {a, b}));
        else if (!sh && commutes(app) && ri(a) == Want::Imm)
            alts.emplace_back(root(bin_, 1, {{b}, {a, Want::Imm}}, {b, a}));

        if (!alu_id(app, false) || s.uses(app->arg()) != 1) return;
        for (auto [v, other] : {std::pair{b, a}, std::pair{a, b}}) {
            if (v == a && !commutes(app)) break;
            auto ld = s.sole_load(v);
            if (!ld || v == other) continue;
            // The replacement sits at the load, so the other operand must be available there.
            auto bb   = chain(ld->arg(2, 0));
            auto done = DefSet();
            if (bb && other->in_scope(bb) && !depends(other, ld, done)) {
                auto alt = root(bin_rm, 1, {{other}, {v, Want::Reg, Load}}, {ld, other});
                alt.covers.emplace_back(Cover{ld, Load, {{ld->arg(2, 1), Want::Addr}}});
                alts.emplace_back(std::move(alt));
            }
        }
    } else if (Axm::isa<core::icmp>(app) || Axm::isa<core::ncmp>(app)) {
        auto ic = Axm::isa<core::icmp>(app);
        auto nc = Axm::isa<core::ncmp>(app);
        if (!(ic ? cc(ic.id()) : cc(nc.id()))) return;
        auto [a, b] = app->args<2>();
        alts.emplace_back(root(cmp_, 2, {{a}, {b}}, {a, b}));
        if (ri(b) == Want::Imm) alts.emplace_back(root(cmp_, 2, {{a}, {b, Want::Imm}}, {a, b}));
    } else if (auto op = Axm::isa<core::nat>(app)) {
        auto [a, b] = app->args<2>();
        auto imm    = op.id() == core::nat::add || op.id() == core::nat::mul || op.id() == core::nat::sub;
        alts.emplace_back(root(nat_, 1, {{a}, {b}}, {a, b}));
        if (imm && ri(b) == Want::Imm)
            alts.emplace_back(root(nat_, 1, {{a}, {b, Want::Imm}}, {a, b}));
        else if (imm && op.id() != core::nat::sub && ri(a) == Want::Imm)
            alts.emplace_back(root(nat_, 1, {{b}, {a, Want::Imm}}, {b, a}));
    } else if (Axm::isa<core::conv>(app) || Axm::isa<core::idx>(app) || Axm::isa<core::idx2nat>(app)
               || Axm::isa<core::bitcast>(app) || Axm::isa<core::rem>(app)) {
        alts.emplace_back(root(unary, 1, {{app->arg()}}));
    } else if (auto op = Axm::isa<core::div>(app)) {
        auto [x, y] = op->arg(2, 1)->projs<2>();
        alts.emplace_back(root(div_, 1, {{x}, {y}}, {x, y}));
    } else if (Axm::isa<mem::mslot>(app)) {
        alts.emplace_back(root(slot_, 1, {}));
    } else if (Axm::isa<mem::assume>(app)) {
        alts.emplace_back(root(assume_, 0, {}));
    }
}

} // namespace

/*
 * Selection
 */

bool ISel::analyze() {
    for (auto mut : old_world().externals().muts())
        count(mut);

    auto defs = DefVec(counted_.begin(), counted_.end());
    std::ranges::sort(defs, [](const Def* a, const Def* b) { return a->gid() < b->gid(); });

    auto nodes = DefVec();
    for (auto def : defs) {
        auto alts = Alts();
        match(*this, def, alts);
        if (alts.empty()) continue;
        assert(alts.front().covers.empty() && "the first alternative must be atomic");
        alts_[def] = std::move(alts);
        nodes.emplace_back(def);
    }

    // Each inner node of a match becomes an alternative of the node it covers; equal ones are one class.
    for (auto def : nodes)
        for (size_t i = 0, e = alts_[def].size(); i != e; ++i)
            for (size_t j = 0, n = alts_[def][i].covers.size(); j != n; ++j) {
                auto cover  = alts_[def][i].covers[j];
                auto& inner = alts_.at(cover.def);
                if (std::ranges::none_of(inner, [&](const Alt& a) { return !a.rule && a.inner == cover.inner; })) {
                    auto alt    = Alt();
                    alt.inner   = cover.inner;
                    alt.demands = std::move(cover.demands);
                    inner.emplace_back(std::move(alt));
                }
            }

    auto graph = pbqp::Graph();
    auto ids   = DefMap<pbqp::NodeId>();
    for (auto def : nodes) {
        auto costs = pbqp::Costs();
        for (const auto& alt : alts_[def]) {
            auto c = alt.cost;
            for (const auto& d : alt.demands)
                if (!alts_.contains(key(d.def))) c += d.inner ? pbqp::Cost::inf() : leaf(d.def, d.want);
            costs.emplace_back(c);
        }
        ids[def] = graph.add(costs);
    }

    for (auto def : nodes) {
        const auto& alts = alts_[def];
        auto operands    = DefVec();
        for (const auto& alt : alts)
            for (const auto& d : alt.demands)
                if (auto op = key(d.def); alts_.contains(op) && std::ranges::find(operands, op) == operands.end())
                    operands.emplace_back(op);
        for (auto op : operands) {
            auto costs = pbqp::Costs();
            for (const auto& alt : alts)
                for (const auto& o : alts_[op]) {
                    pbqp::Cost c = 0;
                    for (const auto& d : alt.demands) {
                        if (key(d.def) != op) continue;
                        if (d.inner)
                            c += !o.rule && o.inner == d.inner ? 0 : pbqp::Cost::inf();
                        else
                            c += o.rule ? conv(o.rule->type, d.want) : pbqp::Cost::inf();
                    }
                    costs.emplace_back(c);
                }
            if (std::ranges::any_of(costs, [](pbqp::Cost c) { return c != 0; }))
                graph.add_edge_costs(ids[def], ids[op], costs);
        }
    }

    auto atomic = pbqp::Choices(graph.num_nodes(), 0);
    assert(!pbqp::evaluate(graph, atomic).is_inf() && "the atomic rules must cover the program");
    auto sol = pbqp::solve(graph);
    old_world().log().d("x86 isel: {} nodes, {} edges, cost {}", graph.num_nodes(), graph.num_edges(),
                        sol.cost.value());
    // RN is a heuristic, but the atomic rules always remain a valid cover.
    const auto& picked = sol.feasible() ? sol.choices : atomic;
    for (auto def : nodes) {
        auto c        = picked[ids[def]];
        choices_[def] = c;
        for (const auto& cover : alts_[def][c].covers)
            roots_[cover.def] = def;
    }
    return false;
}

void ISel::count(const Def* def) {
    if (!counted_.emplace(def).second) return;
    for (auto op : def->ops()) {
        // An external declaration has no body.
        if (!op) continue;
        ++uses_[op];
        count(op);
    }
}

const App* ISel::sole_load(const Def* def) const {
    if (auto ex = def->isa<Extract>(); ex && uses(def) == 1 && Lit::isa(ex->index()) == 1)
        if (auto ld = ex->tuple()->isa<App>(); ld && Axm::isa<mem::load>(ld) && uses(ld) == 2)
            if (auto m = ld->proj(2, 0); m && uses(m) != 0) return ld;
    return nullptr;
}

const Def* ISel::key(const Def* def) const {
    if (auto ex = def->isa<Extract>(); ex && Lit::isa(ex->index()) == 1)
        if (auto ld = ex->tuple()->isa<App>(); ld && Axm::isa<mem::load>(ld)) return ld;
    return def;
}

const Alt* ISel::choice(const Def* def) const {
    if (auto c = choices_.find(def); c != choices_.end()) return &alts_.at(def)[c->second];
    return nullptr;
}

/*
 * Replacement
 */

const Def* ISel::select(const Def* def) {
    if (auto i = repls_.find(def); i != repls_.end()) return i->second;
    auto alt = choice(def);
    auto res = alt->rule->build(*this, def, *alt);
    if (!res) def->blame(MIM_X86_BE "rule `{}` cannot select `{}`", alt->rule->name, def).bail();
    return repls_[def] = res;
}

const Def* ISel::selected(const Def* def) {
    auto alt = choice(def);
    if (!alt) return nullptr;
    if (alt->rule) return convert(select(def), alt->rule->type, Want::Reg);
    // The root of the match covering this inner node builds it.
    rewrite(roots_.at(def));
    return lookup(def);
}

const Def* ISel::hole(const Def* def, Want want) {
    if (auto alt = choice(def); alt && alt->rule && alt->rule->type != Ty::Reg)
        return convert(select(def), alt->rule->type, want);
    return convert(val(def), Ty::Reg, want);
}

const Def* ISel::convert(const Def* x, Ty from, Want want) {
    auto& w = new_world();
    if (from == Ty::Reg) return want == Want::Reg ? reg(w, x) : want == Want::Imm ? x : addr({x});
    auto a = split(x);
    if (want == Want::Reg || want == Want::Imm) return reg(w, materialize(a));
    return want == Want::Base && a.index ? addr({materialize(a)}) : x;
}

const Def* ISel::zext(const Def* d, const Def* x) {
    if (auto l = Lit::isa(x)) return new_world().lit(new_world().type_idx(d), *l);
    return new_world().call<movzx>(d, reg(new_world(), x));
}

const Def* ISel::val(const Def* def) {
    auto res = rewrite(def);
    if (auto l = Lit::isa(res); l && res->type()->isa<Nat>()) return new_world().lit_i64(*l);
    return res;
}

ISel::Address ISel::split(const Def* addr) {
    auto ix           = Axm::isa<index>(addr);
    auto [disp, base] = Axm::as<x86::addr>(ix ? ix->decurry()->arg() : addr)->args<2>();
    auto a            = Address{base, nullptr, 1, Lit::as<s32>(disp)};
    if (ix) {
        auto [i, scale] = ix->args<2>();
        a.index         = i;
        a.scale         = 1_n << *Lit::isa(scale);
    }
    return a;
}

const Def* ISel::addr(Address a) {
    auto& w = new_world();
    if (auto l = Lit::isa(a.base)) a.disp += *l, a.base = w.lit_i64(0);
    if (a.index)
        if (auto l = Lit::isa(a.index)) a.disp += *l * a.scale, a.index = nullptr;
    if (!fits_i32(a.disp)) a = {materialize(a)};
    auto res = w.call<x86::addr>(Defs{w.lit_i32(u32(a.disp)), a.base});
    return a.index ? w.call<index>(res, Defs{a.index, w.lit_i2(std::countr_zero(a.scale))}) : res;
}

const Def* ISel::materialize(Address a) {
    auto& w = new_world();
    if (!a.index && Lit::isa(a.base)) return w.lit_i64(*Lit::isa(a.base) + a.disp);
    if (!fits_i32(a.disp)) a = {bin<add>(w, a.base, w.lit_i64(a.disp)), a.index, a.scale};
    if (!a.index && a.disp == 0) return a.base;
    return w.call<lea>(addr(a));
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
    if (auto res = selected(ex)) return res;
    return RWPhase::rewrite_imm_Extract(ex);
}

const Def* ISel::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (Axm::isa<core::Ptr>(app)) return w.type_i64();
    if (Axm::isa<core::M>(app)) return w.call<core::M>(w.lit_nat_0());
    if (auto size = Idx::isa(app); size && !Lit::isa(size)) return w.type_i64();
    if (auto res = selected(app)) return res;

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !Axm::isa<core::M>(app)
                               && !app->type()->isa<Type>() && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
