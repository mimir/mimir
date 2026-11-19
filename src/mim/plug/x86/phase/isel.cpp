#include "mim/plug/x86/phase/isel.h"

#include <array>
#include <print>

#include <pbqp/dot.h>
#include <pbqp/solver.h>

#include <mim/plugin.h>

#include <mim/plug/core/core.h>
#include <mim/plug/ll/autogen.h>
#include <mim/plug/math/math.h>
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
    // clang-format off
    switch (id) {
        case core::icmp::  e: return 4;
        case core::icmp:: ne: return 5;
        case core::icmp::ul : return 2;
        case core::icmp::ule: return 6;
        case core::icmp::ug : return 7;
        case core::icmp::uge: return 3;
        case core::icmp::sl : return 12;
        case core::icmp::sle: return 14;
        case core::icmp::sg : return 15;
        case core::icmp::sge: return 13;
        default: return {};
    }
    // clang-format on
}

std::optional<nat_t> cc(core::ncmp id) {
    // clang-format off
    switch (id) {
        case core::ncmp:: e: return 4;
        case core::ncmp::ne: return 5;
        case core::ncmp::l : return 2;
        case core::ncmp::le: return 6;
        case core::ncmp::g : return 7;
        case core::ncmp::ge: return 3;
        default: return {};
    }
    // clang-format on
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
    return rr<Id>(w, a, b)->proj(2, I);
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
    return p == core::Plugin_Id || p == mem::Plugin_Id || p == math::Plugin_Id;
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

} // namespace

const Def* ISel::wide(Insn insn, const Def* type, bool sign, const Def* a, const Def* b) {
    auto& w   = new_world();
    auto size = Idx::isa_lit(type);
    if (!size || reg_width(*size) >= 16) return insn(w, a, b);
    auto ext = [&](const Def* x) { return sign ? w.call<movsx>(w.lit_i32(), reg(w, x)) : zext(w.lit_i32(), x); };
    return w.call<trunc>(*size, insn(w, ext(a), ext(b)));
}

namespace {

/*
 * Costs
 */

/// Cost of converting a replacement of type @p from into a hole of type @p want.
pbqp::Cost conv(Ty from, Want want) {
    switch (want) {
        case Want::Reg: return from == Ty::Reg ? 0 : 1;
        case Want::Imm: return pbqp::Cost::inf();
        case Want::Sh: return pbqp::Cost::inf();
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

/// Does @p def satisfy a hole of type @p want that requires a literal?
bool literal(const Def* def, Want want) { return Lit::isa(def) && (want == Want::Sh || fits_imm(def)); }

/*
 * Rules
 */

constexpr u64 Load = 1; ///< The inner node `mem.load (m, p: Addr i)`.
constexpr u64 Alu  = 2; ///< The inner node of the value a `store_mr` folds into.

template<class... W>
constexpr u16 pack(W... w) {
    u16 res  = 0;
    size_t i = 0;
    ((res |= u16(w) << (3 * i++)), ...);
    return res;
}

template<class... W>
constexpr Rule rule(Op op,
                    Form form,
                    Ty type,
                    pbqp::Cost cost,
                    std::string_view name,
                    const Def* (ISel::*build)(const Def*, const Alt&),
                    W... w) {
    return Rule{op, form, type, cost, u8(sizeof...(W)), pack(w...), name, build};
}

/// An atomic rule has no inner nodes and only holes that every rule type can fill.
constexpr bool atomic(const Rule& r) {
    if (r.form != Form::Reg) return false;
    for (size_t i = 0; i != r.num_holes; ++i) {
        auto want = r.want(i);
        if (want != Want::Reg && want != Want::Base && want != Want::Addr) return false;
    }
    return true;
}

constexpr std::string_view op_name(Op op) {
    switch (op) {
#define X(name) \
    case Op::name: return #name;
        MIM_X86_ISEL_OPS(X)
#undef X
    }
    std::unreachable();
}

// clang-format off
/// The rule set: one entry per pattern ↦ replacement.
constexpr Rule rules_[] = {
    // An address is an operand form, not a value.
    rule(Op::LeaOffset, Form::Reg, Ty::Base,  0, "lea_off_base",  &ISel::build_lea_off, Want::Base),
    rule(Op::LeaOffset, Form::Reg, Ty::Index, 0, "lea_off_index", &ISel::build_lea_off, Want::Index),
    rule(Op::LeaScale,  Form::Reg, Ty::Index, 0, "lea_index",     &ISel::build_lea_idx, Want::Base, Want::Reg),
    rule(Op::LeaMul,    Form::Reg, Ty::Index, 1, "lea_index_mul", &ISel::build_lea_idx, Want::Base, Want::Reg),

    rule(Op::Load,  Form::Reg, Ty::Reg, 1, "load",      &ISel::build_load, Want::Addr),
    rule(Op::Store, Form::Reg, Ty::Reg, 1, "store",     &ISel::build_store, Want::Addr, Want::Reg),
    rule(Op::Store, Form::Imm, Ty::Reg, 1, "store_imm", &ISel::build_store, Want::Addr, Want::Imm),
    rule(Op::Store, Form::Rm,  Ty::Reg, 1, "store_mr",  &ISel::build_store_mr),

    rule(Op::WrapAdd, Form::Reg, Ty::Reg, 1, "add",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapAdd, Form::Imm, Ty::Reg, 1, "add_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapAdd, Form::Rm,  Ty::Reg, 1, "add_rm",    &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::WrapSub, Form::Reg, Ty::Reg, 1, "sub",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapSub, Form::Imm, Ty::Reg, 1, "sub_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapSub, Form::Rm,  Ty::Reg, 1, "sub_rm",    &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::WrapMul, Form::Reg, Ty::Reg, 1, "mul",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapMul, Form::Imm, Ty::Reg, 1, "mul_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapMul, Form::Rm,  Ty::Reg, 1, "mul_rm",    &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::WrapShl, Form::Reg, Ty::Reg, 1, "shl",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapShl, Form::Imm, Ty::Reg, 1, "shl_imm",   &ISel::build_bin, Want::Reg, Want::Sh),
    rule(Op::BitAnd,  Form::Reg, Ty::Reg, 1, "and",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitAnd,  Form::Imm, Ty::Reg, 1, "and_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitAnd,  Form::Rm,  Ty::Reg, 1, "and_rm",    &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::BitOr,   Form::Reg, Ty::Reg, 1, "or",        &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitOr,   Form::Imm, Ty::Reg, 1, "or_imm",    &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitOr,   Form::Rm,  Ty::Reg, 1, "or_rm",     &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::BitXor,  Form::Reg, Ty::Reg, 1, "xor",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitXor,  Form::Imm, Ty::Reg, 1, "xor_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitXor,  Form::Rm,  Ty::Reg, 1, "xor_rm",    &ISel::build_bin_rm, Want::Reg, Want::Reg),
    rule(Op::BitNand, Form::Reg, Ty::Reg, 1, "nand",      &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNand, Form::Imm, Ty::Reg, 1, "nand_imm",  &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitNor,  Form::Reg, Ty::Reg, 1, "nor",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNor,  Form::Imm, Ty::Reg, 1, "nor_imm",   &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitNxor, Form::Reg, Ty::Reg, 1, "nxor",      &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNxor, Form::Imm, Ty::Reg, 1, "nxor_imm",  &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::ShrL,    Form::Reg, Ty::Reg, 1, "shr",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::ShrL,    Form::Imm, Ty::Reg, 1, "shr_imm",   &ISel::build_bin, Want::Reg, Want::Sh),
    rule(Op::ShrA,    Form::Reg, Ty::Reg, 1, "sar",       &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::ShrA,    Form::Imm, Ty::Reg, 1, "sar_imm",   &ISel::build_bin, Want::Reg, Want::Sh),

    rule(Op::Icmp, Form::Reg, Ty::Reg, 2, "cmp",     &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Icmp, Form::Imm, Ty::Reg, 2, "cmp_imm", &ISel::build_cmp, Want::Reg, Want::Imm),
    rule(Op::Ncmp, Form::Reg, Ty::Reg, 2, "ncmp",     &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Ncmp, Form::Imm, Ty::Reg, 2, "ncmp_imm", &ISel::build_cmp, Want::Reg, Want::Imm),

    rule(Op::NatAdd, Form::Reg, Ty::Reg, 1, "nat_add",     &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatAdd, Form::Imm, Ty::Reg, 1, "nat_add_imm", &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatSub, Form::Reg, Ty::Reg, 1, "nat_sub",     &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatSub, Form::Imm, Ty::Reg, 1, "nat_sub_imm", &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatMul, Form::Reg, Ty::Reg, 1, "nat_mul",     &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatMul, Form::Imm, Ty::Reg, 1, "nat_mul_imm", &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatDiv, Form::Reg, Ty::Reg, 1, "nat_div",     &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatRem, Form::Reg, Ty::Reg, 1, "nat_rem",     &ISel::build_nat, Want::Reg, Want::Reg),

    rule(Op::Conv,    Form::Reg, Ty::Reg, 1, "conv",     &ISel::build_unary, Want::Reg),
    rule(Op::Idx,     Form::Reg, Ty::Reg, 1, "idx",      &ISel::build_unary, Want::Reg),
    rule(Op::Idx2Nat, Form::Reg, Ty::Reg, 1, "idx2nat",  &ISel::build_unary, Want::Reg),
    rule(Op::Bitcast, Form::Reg, Ty::Reg, 1, "bitcast",  &ISel::build_unary, Want::Reg),
    rule(Op::Rem,     Form::Reg, Ty::Reg, 1, "rem",      &ISel::build_unary, Want::Reg),

    rule(Op::Div,    Form::Reg, Ty::Reg, 1, "div",    &ISel::build_div, Want::Reg, Want::Reg),
    rule(Op::Slot,   Form::Reg, Ty::Reg, 1, "slot",   &ISel::build_slot),
    rule(Op::Assume, Form::Reg, Ty::Reg, 0, "assume", &ISel::build_assume),
    rule(Op::Select, Form::Reg, Ty::Reg, 1, "select", &ISel::build_select, Want::Reg, Want::Reg, Want::Reg),
};
// clang-format on

/// @p op is a shift, whose immediate operand may be any literal.
bool is_shift(Op op) { return op == Op::WrapShl || op == Op::ShrA || op == Op::ShrL; }

std::optional<Op> bin_op(const Def* def) {
    if (auto w = Axm::isa<core::wrap>(def)) {
        switch (w.id()) {
            case core::wrap::add: return Op::WrapAdd;
            case core::wrap::sub: return Op::WrapSub;
            case core::wrap::mul: return Op::WrapMul;
            case core::wrap::shl: return Op::WrapShl;
        }
        return {};
    }
    if (auto b = Axm::isa<core::bit2>(def)) {
        switch (b.id()) {
            case core::bit2::and_: return Op::BitAnd;
            case core::bit2::or_: return Op::BitOr;
            case core::bit2::xor_: return Op::BitXor;
            case core::bit2::nand: return Op::BitNand;
            case core::bit2::nor: return Op::BitNor;
            case core::bit2::nxor: return Op::BitNxor;
            default: return {};
        }
    }
    if (auto s = Axm::isa<core::shr>(def)) return s.id() == core::shr::a ? Op::ShrA : Op::ShrL;
    return {};
}

Op nat_op(core::nat id) {
    switch (id) {
        case core::nat::add: return Op::NatAdd;
        case core::nat::sub: return Op::NatSub;
        case core::nat::mul: return Op::NatMul;
        case core::nat::div: return Op::NatDiv;
        case core::nat::rem: return Op::NatRem;
    }
    std::unreachable();
}

std::optional<Op> unary_op(const Def* def) {
    if (Axm::isa<core::conv>(def)) return Op::Conv;
    if (Axm::isa<core::idx>(def)) return Op::Idx;
    if (Axm::isa<core::idx2nat>(def)) return Op::Idx2Nat;
    if (Axm::isa<core::bitcast>(def)) return Op::Bitcast;
    if (Axm::isa<core::rem>(def)) return Op::Rem;
    return {};
}

Want ri(const Def* def) { return Lit::isa(def) && fits_imm(def) ? Want::Imm : Want::Reg; }

} // namespace

const Def* ISel::build_lea_off(const Def* root, const Alt& alt) {
    auto a = split(hole(alt.demands[0].def, alt.demands[0].want));
    a.disp += *offset(root->as<App>());
    return addr(a);
}

const Def* ISel::build_lea_idx(const Def* root, const Alt& alt) {
    auto& w   = new_world();
    auto i    = alt.demands[1].def;
    auto a    = split(hole(alt.demands[0].def, Want::Base));
    auto size = elem_size(root->as<App>());
    auto x    = hole(i);
    if (auto n = Idx::isa_lit(i->type()); n && *n != 0) x = zext(w.lit_i64(), x);
    if (size == 1 || size == 2 || size == 4 || size == 8) return addr({a.base, x, u8(size), a.disp});
    return addr({a.base, bin<imul>(w, x, w.lit_i64(size)), 1, a.disp});
}

const Def* ISel::build_load(const Def* root, const Alt&) {
    auto [m, p] = root->as<App>()->args<2>();
    auto size   = *xsize(root->type()->proj(2, 1));
    return new_world().call<load>(size, Defs{rewrite(m), hole(p, Want::Addr)});
}

const Def* ISel::build_store(const Def* root, const Alt& alt) {
    auto [m, p, x] = root->as<App>()->args<3>();
    return new_world().call<store>(Defs{rewrite(m), hole(p, Want::Addr), hole(x, alt.demands[1].want)});
}

const Def* ISel::build_store_mr(const Def* root, const Alt& alt) {
    auto& w        = new_world();
    auto [_, p, u] = root->as<App>()->args<3>();
    auto ld        = alt.covers[1].def;
    auto other     = alt.covers[0].demands[1];
    auto b         = hole(other.def, other.want);
    return w.call<mr>(alu(w, u), Defs{rewrite(ld->as<App>()->arg(2, 0)), hole(p, Want::Addr), b});
}

const Def* ISel::build_bin(const Def* root, const Alt& alt) {
    auto& w  = new_world();
    auto app = root->as<App>();
    auto a   = hole(alt.demands[0].def, alt.demands[0].want);
    auto b   = hole(alt.demands[1].def, alt.demands[1].want);
    if (auto op = Axm::isa<core::wrap>(app)) {
        switch (op.id()) {
            case core::wrap::add: return bin<add>(w, a, b);
            case core::wrap::sub: return bin<sub>(w, a, b);
            // The low bits of a product do not depend on how the operands were extended.
            case core::wrap::mul: return wide(bin<imul>, app->type(), false, a, b);
            case core::wrap::shl: return shift<shl>(w, a, b);
        }
    } else if (auto op = Axm::isa<core::bit2>(app)) {
        // clang-format off
        switch (op.id()) {
            case core::bit2::and_: return bin<and_>(w, a, b);
            case core::bit2::or_:  return bin<or_> (w, a, b);
            case core::bit2::xor_: return bin<xor_>(w, a, b);
            case core::bit2::nand: return bin<xor_>(w, bin<and_>(w, a, b), ones(w, a->type()));
            case core::bit2::nor:  return bin<xor_>(w, bin<or_ >(w, a, b), ones(w, a->type()));
            case core::bit2::nxor: return bin<xor_>(w, bin<xor_>(w, a, b), ones(w, a->type()));
            default: return nullptr;
        }
        // clang-format on
    }
    auto op = Axm::as<core::shr>(app);
    return op.id() == core::shr::a ? shift<sar>(w, a, b) : shift<shr>(w, a, b);
}

const Def* ISel::build_bin_rm(const Def* root, const Alt& alt) {
    auto& w     = new_world();
    auto ld     = alt.covers[0].def->as<App>();
    auto [m, p] = ld->args<2>();
    auto res    = w.call<rm>(alu(w, root), Defs{rewrite(m), hole(alt.demands[0].def), hole(p, Want::Addr)});
    map(ld, res);
    return res->proj(2, 1);
}

const Def* ISel::build_cmp(const Def* root, const Alt& alt) {
    auto& w   = new_world();
    auto a    = hole(alt.demands[0].def);
    auto b    = hole(alt.demands[1].def, alt.demands[1].want);
    auto op   = Axm::isa<core::icmp>(root);
    auto code = op ? cc(op.id()) : cc(Axm::as<core::ncmp>(root).id());
    return w.call<x86::cc>(*code, bin<cmp>(w, a, b));
}

const Def* ISel::build_nat(const Def* root, const Alt& alt) {
    auto& w = new_world();
    auto a  = hole(alt.demands[0].def);
    auto b  = hole(alt.demands[1].def, alt.demands[1].want);
    switch (Axm::as<core::nat>(root).id()) {
        case core::nat::add: return bin<add>(w, a, b);
        case core::nat::mul: return bin<imul>(w, a, b);
        case core::nat::sub: return w.call<cmovcc>(2, Defs{bin<cmp>(w, a, b), bin<sub>(w, a, b), reg(w, w.lit_i64(0))});
        case core::nat::div: return divrem<div, 0>(w, a, b);
        case core::nat::rem: return divrem<div, 1>(w, a, b);
    }
    std::unreachable();
}

const Def* ISel::build_unary(const Def* root, const Alt& alt) {
    auto& w  = new_world();
    auto app = root->as<App>();
    auto x   = hole(alt.demands[0].def);
    if (auto op = Axm::isa<core::conv>(app)) {
        auto src = Idx::isa_lit(x->type());
        auto dst = Idx::isa_lit(rewrite(app->type()));
        if (!src || !dst) return nullptr;
        if (*src == *dst) return x;
        if (reg_width(*src) >= reg_width(*dst)) return w.call<trunc>(*dst, x);
        return op.id() == core::conv::s ? w.call<movsx>(*dst, x) : w.call<movzx>(*dst, x);
    } else if (Axm::isa<core::idx>(app)) {
        return rewrite(app->type()) == w.type_i64() ? x : nullptr;
    } else if (Axm::isa<core::idx2nat>(app)) {
        return Idx::isa_lit(x->type()) == 0 ? x : zext(w.lit_i64(), x);
    } else if (Axm::isa<core::bitcast>(app)) {
        auto src = Idx::isa_lit(x->type());
        auto dst = Idx::isa_lit(rewrite(app->type()));
        if (x->type() == rewrite(app->type())) return x;
        if (!src || !dst) return nullptr;
        return reg_width(*src) >= reg_width(*dst) ? w.call<trunc>(*dst, x) : w.call<movzx>(*dst, x);
    }
    auto op = Axm::as<core::rem>(app);
    auto d  = w.lit(x->type(), *Lit::isa(op->decurry()->arg()));
    return wide(divrem<div, 1>, x->type(), false, x, d);
}

const Def* ISel::build_div(const Def* root, const Alt& alt) {
    auto& w   = new_world();
    auto op   = Axm::as<core::div>(root);
    auto x    = hole(alt.demands[0].def);
    auto y    = hole(alt.demands[1].def);
    auto type = x->type();
    const Def* res;
    switch (op.id()) {
        case core::div::udiv: res = wide(divrem<div, 0>, type, false, x, y); break;
        case core::div::urem: res = wide(divrem<div, 1>, type, false, x, y); break;
        case core::div::sdiv: res = wide(divrem<idiv, 0>, type, true, x, y); break;
        case core::div::srem: res = wide(divrem<idiv, 1>, type, true, x, y); break;
        default: std::unreachable();
    }
    return w.tuple({rewrite(op->arg(2, 0)), res});
}

const Def* ISel::build_slot(const Def* root, const Alt&) {
    auto& w        = new_world();
    auto op        = Axm::as<mem::mslot>(root);
    auto [T, _]    = op->decurry()->args<2>();
    auto [msz, k]  = op->args<2>();
    auto [m, size] = msz->projs<2>();
    auto n         = Lit::isa(size).value_or(trait(core::trait::size, T));
    return w.app(rewrite(k), w.call<slot>(n, rewrite(m)));
}

const Def* ISel::build_assume(const Def* root, const Alt&) { return rewrite(root->as<App>()->arg(2, 0)); }

const Def* ISel::build_select(const Def* root, const Alt& alt) {
    auto& w   = new_world();
    auto size = *xsize(root->type());
    auto x    = hole(alt.demands[0].def);
    auto y    = hole(alt.demands[1].def);
    auto c    = hole(alt.demands[2].def);
    auto code = 5_n;
    auto fl   = rr<test>(w, c, c);
    if (auto app = c->isa<App>(); app && app->axm() && app->axm()->flags() == Annex::Base<x86::cc>)
        code = *Lit::isa(app->callee()->as<App>()->arg()), fl = app->arg();
    // `cmov` has no 8-bit form.
    if (reg_width(size) < 16) {
        auto ext = [&](const Def* v) { return reg(w, zext(w.lit_i32(), v)); };
        return w.call<trunc>(size, w.call<cmovcc>(code, Defs{fl, ext(x), ext(y)}));
    }
    return w.call<cmovcc>(code, Defs{fl, x, y});
}

size_t ISel::append(Op op, Form form, Alts& alts, Defs operands) const {
    size_t n = 0;
    for (const auto& rule : rules_) {
        if (rule.op != op || rule.form != form) continue;
        assert(operands.size() == rule.num_holes);
        auto alt = Alt();
        alt.rule = &rule;
        alt.cost = rule.cost;
        bool ok  = true;
        for (size_t i = 0; ok && i != rule.num_holes; ++i) {
            auto want = rule.want(i);
            if ((want == Want::Imm || want == Want::Sh) && !literal(operands[i], want))
                ok = false;
            else
                alt.demands.emplace_back(Demand{operands[i], want});
        }
        if (ok) {
            alts.emplace_back(std::move(alt));
            ++n;
        }
    }
    return n;
}

void ISel::append_bin_rm(Op op, Alts& alts, const Def* other, const Def* v, const App* ld) const {
    for (const auto& rule : rules_) {
        if (rule.op != op || rule.form != Form::Rm) continue;
        auto alt = Alt();
        alt.rule = &rule;
        alt.cost = rule.cost;
        alt.demands.emplace_back(Demand{other, Want::Reg});
        alt.demands.emplace_back(Demand{v, Want::Reg, Load});
        alt.covers.emplace_back(Cover{ld, Load, {{ld->arg(2, 1), Want::Addr}}});
        alts.emplace_back(std::move(alt));
    }
}

void ISel::append_store_mr(Alts& alts,
                           const Def* p,
                           const Def* x,
                           const App* u,
                           const App* ld,
                           const Def* v,
                           const Def* other) const {
    for (const auto& rule : rules_) {
        if (rule.op != Op::Store || rule.form != Form::Rm) continue;
        auto alt = Alt();
        alt.rule = &rule;
        alt.cost = rule.cost;
        alt.demands.emplace_back(Demand{p, Want::Addr});
        alt.demands.emplace_back(Demand{x, Want::Reg, Alu});
        alt.covers.emplace_back(Cover{u, Alu, {{v, Want::Reg, Load}, {other, ri(other)}}});
        alt.covers.emplace_back(Cover{ld, Load, {{ld->arg(2, 1), Want::Addr}}});
        alts.emplace_back(std::move(alt));
    }
}

/// Appends the matches rooted at @p def to @p alts.
void ISel::match(const Def* def, Alts& alts) const {
    if (auto ex = def->isa<Extract>()) {
        auto index = ex->index();
        if (xsize(ex->type()) && Idx::isa_lit(index->type()) == 2 && !Lit::isa(index)
            && ex->tuple()->num_projs() == 2) {
            auto [x, y] = ex->tuple()->projs<2>();
            append(Op::Select, Form::Reg, alts, {x, y, index});
        }
        return;
    }

    auto app = def->isa<App>();
    if (!app) return;

    if (auto lea = Axm::isa<mem::lea>(app)) {
        auto [p, i] = lea->args<2>();
        if (offset(app)) {
            append(Op::LeaOffset, Form::Reg, alts, {p});
        } else {
            auto size = elem_size(app);
            append(size == 1 || size == 2 || size == 4 || size == 8 ? Op::LeaScale : Op::LeaMul, Form::Reg, alts,
                   {p, i});
        }
    } else if (auto ld = Axm::isa<mem::load>(app)) {
        if (xsize(app->type()->proj(2, 1))) append(Op::Load, Form::Reg, alts, {ld->arg(2, 1)});
    } else if (auto st = Axm::isa<mem::store>(app)) {
        auto [m, p, x] = st->args<3>();
        if (!xsize(x->type())) return;
        append(Op::Store, Form::Reg, alts, {p, x});
        append(Op::Store, Form::Imm, alts, {p, x});
        if (auto u = x->isa<App>(); u && uses(u) == 1 && alu_id(u, true) && uses(u->arg()) == 1) {
            auto [a, b] = u->args<2>();
            for (auto [v, other, v_is_a] : {std::tuple{a, b, true}, std::tuple{b, a, false}}) {
                if (!v_is_a && !commutes(u)) break;
                // The load's memory has no other user, so nothing lies between load and store.
                if (auto ld = sole_load(v);
                    ld && v != other && ld->arg(2, 1) == p && ld->proj(2, 0) == m && uses(m) == 1)
                    append_store_mr(alts, p, x, u, ld, v, other);
            }
        }
    } else if (auto op = bin_op(app)) {
        auto [a, b] = app->args<2>();
        auto sh     = is_shift(*op);
        append(*op, Form::Reg, alts, {a, b});
        if (!append(*op, Form::Imm, alts, {a, b}) && !sh && commutes(app)) append(*op, Form::Imm, alts, {b, a});

        if (!alu_id(app, false) || uses(app->arg()) != 1) return;
        for (auto [v, other, v_is_a] : {std::tuple{b, a, false}, std::tuple{a, b, true}}) {
            if (v_is_a && !commutes(app)) break;
            auto ld = sole_load(v);
            if (!ld || v == other) continue;
            // The replacement sits at the load, so the other operand must be available there.
            auto bb   = chain(ld->arg(2, 0));
            auto done = DefSet();
            if (bb && other->in_scope(bb) && !depends(other, ld, done)) append_bin_rm(*op, alts, other, v, ld);
        }
    } else if (auto ic = Axm::isa<core::icmp>(app)) {
        if (!cc(ic.id())) return;
        auto [a, b] = app->args<2>();
        append(Op::Icmp, Form::Reg, alts, {a, b});
        append(Op::Icmp, Form::Imm, alts, {a, b});
    } else if (auto nc = Axm::isa<core::ncmp>(app)) {
        if (!cc(nc.id())) return;
        auto [a, b] = app->args<2>();
        append(Op::Ncmp, Form::Reg, alts, {a, b});
        append(Op::Ncmp, Form::Imm, alts, {a, b});
    } else if (auto n = Axm::isa<core::nat>(app)) {
        auto op     = nat_op(n.id());
        auto [a, b] = app->args<2>();
        append(op, Form::Reg, alts, {a, b});
        if (op != Op::NatDiv && op != Op::NatRem)
            if (!append(op, Form::Imm, alts, {a, b}) && op != Op::NatSub) append(op, Form::Imm, alts, {b, a});
    } else if (auto op = unary_op(app)) {
        append(*op, Form::Reg, alts, {app->arg()});
    } else if (auto op = Axm::isa<core::div>(app)) {
        auto [x, y] = op->arg(2, 1)->projs<2>();
        append(Op::Div, Form::Reg, alts, {x, y});
    } else if (Axm::isa<mem::mslot>(app)) {
        append(Op::Slot, Form::Reg, alts, Defs{});
    } else if (Axm::isa<mem::assume>(app)) {
        append(Op::Assume, Form::Reg, alts, Defs{});
    }
}

/*
 * Selection
 */

bool ISel::analyze() {
#ifdef MIM_ENABLE_CHECKS
    check_rules();
#endif
    for (auto mut : old_world().externals().muts())
        count(mut);

    auto defs = DefVec(counted_.begin(), counted_.end());
    std::ranges::sort(defs, [](const Def* a, const Def* b) { return a->gid() < b->gid(); });

    auto nodes = DefVec();
    for (auto def : defs) {
        auto alts = Alts();
        match(def, alts);
        if (alts.empty()) continue;
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
            const auto& op_alts = alts_[op];
            auto costs          = pbqp::Matrix(alts.size(), op_alts.size(), [&](size_t i, size_t j) {
                const auto& o = op_alts[j];
                auto c        = pbqp::Cost(0);
                for (const auto& d : alts[i].demands) {
                    if (key(d.def) != op) continue;
                    if (d.inner)
                        c += !o.rule && o.inner == d.inner ? 0 : pbqp::Cost::inf();
                    else
                        c += o.rule ? conv(o.rule->type, d.want) : pbqp::Cost::inf();
                }
                return c;
            });
            if (std::ranges::any_of(costs, [](pbqp::Cost c) { return c != 0; }))
                graph.add_edge_costs(ids[def], ids[op], costs);
        }
    }

    auto atomic = pbqp::Choices(graph.num_nodes(), 0);
    for (auto def : nodes) {
        auto i           = atomic_choice(alts_[def]);
        atomic[ids[def]] = i == size_t(-1) ? 0 : i;
#ifdef MIM_ENABLE_CHECKS
        if (i == size_t(-1)) fe::throwf("x86 isel: `{}` has no atomic rule", def);
#endif
    }
#ifdef MIM_ENABLE_CHECKS
    if (pbqp::evaluate(graph, atomic).is_inf()) fe::throwf("the atomic rules must cover the program");
#endif
    auto sol = pbqp::solve(graph);
    log().d("x86 isel: {} nodes, {} edges, cost {}", graph.num_nodes(), graph.num_edges(), sol.cost.value());
    // RN is a heuristic, but the atomic rules always remain a valid cover.
    const auto& picked = sol.feasible() ? sol.choices : atomic;
    if (auto d = arg_value(args(), "isel-dot")) {
        auto out = Out(std::string(*d));
        auto& os = *out.os();
        for (auto def : nodes)
            std::println(os, "// n{} = {}", ids[def], def);
        pbqp::write_dot(os, graph, picked);
    }
    for (auto def : nodes) {
        auto c        = picked[ids[def]];
        choices_[def] = c;
        for (const auto& cover : alts_[def][c].covers)
            roots_[cover.def] = def;
    }
    return false;
}

size_t ISel::atomic_choice(const Alts& alts) const {
    for (size_t i = 0; i != alts.size(); ++i)
        if (alts[i].rule && atomic(*alts[i].rule)) return i;
    return size_t(-1);
}

#ifdef MIM_ENABLE_CHECKS
void ISel::check_rules() const {
    for (const auto& r : rules_) {
        if (conv(r.type, Want::Reg).is_inf())
            fe::throwf("x86 isel: rule `{}` yields a value that does not convert to a register", r.name);
        for (size_t i = 0; i != r.num_holes; ++i) {
            auto want = r.want(i);
            if (want == Want::Imm || want == Want::Sh) continue; // satisfied by a literal, not a rule type
            bool ok = false;
            for (auto ty : {Ty::Reg, Ty::Base, Ty::Index})
                if (!conv(ty, want).is_inf()) ok = true;
            if (!ok) fe::throwf("x86 isel: no rule type converts to a hole of `{}`", r.name);
        }
    }

    std::array<bool, size_t(Op::Num)> seen{};
    for (const auto& r : rules_) {
        if (seen[size_t(r.op)]) continue;
        seen[size_t(r.op)] = true;
        bool ok            = false;
        for (const auto& x : rules_)
            if (x.op == r.op && atomic(x)) ok = true;
        if (!ok) fe::throwf("x86 isel: no atomic rule for `{}`", op_name(r.op));
    }

    // The only inner nodes the rules span are loads, so `Op::Load` must itself be coverable by a rule.
    for (const auto& r : rules_) {
        if (r.form != Form::Rm) continue;
        bool ok = false;
        for (const auto& x : rules_)
            if (x.op == Op::Load && atomic(x)) ok = true;
        if (!ok) fe::throwf("x86 isel: `{}` folds a load, but no atomic rule selects one", r.name);
    }
}
#endif

void ISel::count(const Def* def) {
    if (!counted_.emplace(def).second) return;
    for (auto op : def->ops()) {
        if (op) {
            ++uses_[op];
            count(op);
        }
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
    auto res = (this->*alt->rule->build)(def, *alt);
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
    if (from == Ty::Reg) return want == Want::Reg ? reg(w, x) : want == Want::Imm || want == Want::Sh ? x : addr({x});
    auto a = split(x);
    if (want == Want::Reg || want == Want::Imm || want == Want::Sh) return reg(w, materialize(a));
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
    // Loop hints for LLVM.
    if (Axm::isa<ll::vec>(app) || Axm::isa<ll::novec>(app)) return rewrite(app->arg());

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !app->type()->isa<Type>()
                               && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
