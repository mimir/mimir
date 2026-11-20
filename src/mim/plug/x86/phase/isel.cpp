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

std::optional<x86::cc> cc(core::icmp id) {
    // clang-format off
    switch (id) {
        case core::icmp::  e: return x86::cc:: e;
        case core::icmp:: ne: return x86::cc::ne;
        case core::icmp::ul : return x86::cc:: b;
        case core::icmp::ule: return x86::cc::be;
        case core::icmp::ug : return x86::cc:: a;
        case core::icmp::uge: return x86::cc::ae;
        case core::icmp::sl : return x86::cc:: l;
        case core::icmp::sle: return x86::cc::le;
        case core::icmp::sg : return x86::cc:: g;
        case core::icmp::sge: return x86::cc::ge;
        default: return {};
    }
    // clang-format on
}

std::optional<x86::cc> cc(core::ncmp id) {
    // clang-format off
    switch (id) {
        case core::ncmp:: e: return x86::cc:: e;
        case core::ncmp::ne: return x86::cc::ne;
        case core::ncmp::l : return x86::cc:: b;
        case core::ncmp::le: return x86::cc::be;
        case core::ncmp::g : return x86::cc:: a;
        case core::ncmp::ge: return x86::cc::ae;
        default: return {};
    }
    // clang-format on
}

std::optional<x86::cc> cc(const Def* cmp) {
    if (auto op = Axm::isa<core::icmp>(cmp)) return cc(op.id());
    return cc(Axm::as<core::ncmp>(cmp).id());
}

/// The `cmovcc` with the same subtag as @p code.
x86::cmovcc cmovcc_of(sub_t code) { return static_cast<x86::cmovcc>(Annex::Base<x86::cmovcc> | code); }

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

/// How `match` extracts the operands of a pattern.
enum class Tag : u8 {
    Lea,   ///< `mem.lea (p, i)`.
    Load,  ///< `mem.load (m, p)`.
    Store, ///< `mem.store (m, p, x)`.
    Bin,   ///< Two operands; the value may come from a folded load.
    Cmp,   ///< Two operands and a supported `core.icmp`/`core.ncmp` predicate.
    Unary, ///< One operand.
    Div,   ///< Two operands behind the `divrem` tuple.
    None,  ///< No operands.
};

/// A program-side operation `match` recognizes: its axiom, the `Op` it selects, and how to take it apart.
struct Pattern {
    flags_t flags;        ///< The axiom, or the whole annex if `member` is `false`.
    bool member = true;   ///< @p flags names one member of the annex.
    Op op;                ///< The operation @p flags selects.
    Tag tag;              ///< How `match` extracts the operands.
    flags_t insn  = 0;    ///< The x86 ALU instruction of its memory form, or 0 if it has none.
    bool commutes = true; ///< An immediate may also be the first operand.
};

/// One member of a multi-member annex.
template<class Id>
constexpr Pattern member(Id id, Op op, Tag tag, flags_t insn = 0, bool commutes = true) {
    return Pattern{flags_t(id), true, op, tag, insn, commutes};
}

/// The whole annex.
template<class Id>
constexpr Pattern annex(Op op, Tag tag) {
    return Pattern{Annex::base<Id>(), false, op, tag, 0, false};
}

// clang-format off
/// The program side of the rule set: which `core`/`mem` axioms isel covers.
constexpr Pattern patterns_[] = {
    // The members of an annex select different instructions.
    member(core::wrap::add,  Op::WrapAdd,  Tag::Bin, Annex::Base<add>),
    member(core::wrap::sub,  Op::WrapSub,  Tag::Bin, Annex::Base<sub>,  false),
    member(core::wrap::mul,  Op::WrapMul,  Tag::Bin, Annex::Base<imul>),
    member(core::wrap::shl,  Op::WrapShl,  Tag::Bin, 0,                false),
    member(core::bit2::and_, Op::BitAnd,   Tag::Bin, Annex::Base<and_>),
    member(core::bit2::or_,  Op::BitOr,    Tag::Bin, Annex::Base<or_>),
    member(core::bit2::xor_, Op::BitXor,   Tag::Bin, Annex::Base<xor_>),
    member(core::bit2::nand, Op::BitNand,  Tag::Bin),
    member(core::bit2::nor,  Op::BitNor,   Tag::Bin),
    member(core::bit2::nxor, Op::BitNxor,  Tag::Bin),
    member(core::shr::a,     Op::ShrA,     Tag::Bin, 0,                false),
    member(core::shr::l,     Op::ShrL,     Tag::Bin, 0,                false),
    member(core::nat::add,   Op::NatAdd,   Tag::Bin),
    member(core::nat::sub,   Op::NatSub,   Tag::Bin, 0,                false),
    member(core::nat::mul,   Op::NatMul,   Tag::Bin),
    member(core::nat::div,   Op::NatDiv,   Tag::Bin),
    member(core::nat::rem,   Op::NatRem,   Tag::Bin),

    // One axiom per instruction.
    annex<core::icmp>    (Op::Icmp,      Tag::Cmp),
    annex<core::ncmp>    (Op::Ncmp,      Tag::Cmp),
    annex<core::conv>    (Op::Conv,      Tag::Unary),
    annex<core::idx>     (Op::Idx,       Tag::Unary),
    annex<core::idx2nat> (Op::Idx2Nat,   Tag::Unary),
    annex<core::bitcast> (Op::Bitcast,   Tag::Unary),
    annex<core::rem>     (Op::Rem,       Tag::Unary),
    annex<core::div>     (Op::Div,       Tag::Div),
    annex<mem::lea>      (Op::LeaOffset, Tag::Lea),
    annex<mem::load>     (Op::Load,      Tag::Load),
    annex<mem::store>    (Op::Store,     Tag::Store),
    annex<mem::mslot>    (Op::Slot,      Tag::None),
    annex<mem::assume>   (Op::Assume,    Tag::None),
};
// clang-format on

/// The pattern of @p def, or `nullptr` if isel does not cover it.
const Pattern* pattern(const Def* def) {
    auto [axm, curry, _] = Axm::get(def);
    if (!axm || curry) return nullptr;
    for (const auto& p : patterns_)
        if (p.member ? axm->flags() == p.flags : axm->base() == p.flags) return &p;
    return nullptr;
}

/// Does @p def, matched by @p p, have a memory-operand form (read-modify-write if @p rmw)?
bool has_mem_form(const Pattern& p, const Def* def, bool rmw) {
    auto s = Idx::isa_lit(def->type());
    if (!p.insn || !s || !is_reg_size(*s)) return false;
    // `imul` has neither an 8-bit nor a read-modify-write form.
    return p.op != Op::WrapMul || (!rmw && reg_width(*s) >= 16);
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
    auto p = pattern(def);
    assert(p && p->insn);
    return w.app(w.annex(p->insn), w.lit_nat(*Idx::isa_lit(def->type())));
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

template<class... S>
constexpr Rule rule(Op op, Ty type, pbqp::Cost cost, std::string_view name, BuildFn build, S... slots) {
    static_assert(sizeof...(S) <= Rule::Max_Slots);
    return Rule{op, type, cost, name, build, Cond::None, u8(sizeof...(S)), {Slot(slots)...}};
}

/// The end of the subtree of slots that starts at @p i, or beyond `num_slots` if the slots are malformed.
constexpr size_t skip(const Rule& r, size_t i) {
    if (i >= r.num_slots) return r.num_slots + 1;
    auto n = r.slots[i++].arity;
    while (n-- != 0)
        i = skip(r, i);
    return i;
}

/// Are the two operand subtrees starting at @p i equal, so that swapping the operands yields no other match?
constexpr bool symmetric(const Rule& r, size_t i) {
    auto j = skip(r, i);
    auto k = skip(r, j);
    return std::equal(r.slots.begin() + i, r.slots.begin() + j, r.slots.begin() + j, r.slots.begin() + k);
}

constexpr bool well_formed(const Rule& r) {
    size_t i = 0;
    while (i < r.num_slots)
        i = skip(r, i);
    return i == r.num_slots;
}

/// An atomic rule has no inner nodes and only holes that every rule type can fill.
constexpr bool atomic(const Rule& r) {
    for (size_t i = 0; i != r.num_slots; ++i) {
        auto s = r.slots[i];
        if (!s.is_hole() || (s.want != Want::Reg && s.want != Want::Base && s.want != Want::Addr)) return false;
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
    rule(Op::LeaOffset, Ty::Base,  0, "lea_off_base",  &ISel::build_lea_off, Want::Base),
    rule(Op::LeaOffset, Ty::Index, 0, "lea_off_index", &ISel::build_lea_off, Want::Index),
    rule(Op::LeaScale,  Ty::Index, 0, "lea_index",     &ISel::build_lea_idx, Want::Base, Want::Reg),
    rule(Op::LeaMul,    Ty::Index, 1, "lea_index_mul", &ISel::build_lea_idx, Want::Base, Want::Reg),

    rule(Op::Load,      Ty::Reg,   1, "load",          &ISel::build_load, Want::Addr),
    rule(Op::Store,     Ty::Reg,   1, "store",         &ISel::build_store, Want::Addr, Want::Reg),
    rule(Op::Store,     Ty::Reg,   1, "store_imm",     &ISel::build_store, Want::Addr, Want::Imm),
    rule(Op::Store,     Ty::Reg,   1, "store_mr",      &ISel::build_store_mr, Want::Addr, Slot(Op::Alu, 2), Slot(Op::Load, 1), Want::Addr, Want::Reg).with(Cond::Rmw),
    rule(Op::Store,     Ty::Reg,   1, "store_mr_imm",  &ISel::build_store_mr, Want::Addr, Slot(Op::Alu, 2), Slot(Op::Load, 1), Want::Addr, Want::Imm).with(Cond::Rmw),

    rule(Op::WrapAdd,   Ty::Reg,   1, "add",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapAdd,   Ty::Reg,   1, "add_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapAdd,   Ty::Reg,   1, "add_rm",        &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::WrapSub,   Ty::Reg,   1, "sub",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapSub,   Ty::Reg,   1, "sub_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapSub,   Ty::Reg,   1, "sub_rm",        &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::WrapMul,   Ty::Reg,   1, "mul",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapMul,   Ty::Reg,   1, "mul_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::WrapMul,   Ty::Reg,   1, "mul_rm",        &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::WrapShl,   Ty::Reg,   1, "shl",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::WrapShl,   Ty::Reg,   1, "shl_imm",       &ISel::build_bin, Want::Reg, Want::Sh),
    rule(Op::BitAnd,    Ty::Reg,   1, "and",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitAnd,    Ty::Reg,   1, "and_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitAnd,    Ty::Reg,   1, "and_rm",        &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::BitOr,     Ty::Reg,   1, "or",            &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitOr,     Ty::Reg,   1, "or_imm",        &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitOr,     Ty::Reg,   1, "or_rm",         &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::BitXor,    Ty::Reg,   1, "xor",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitXor,    Ty::Reg,   1, "xor_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitXor,    Ty::Reg,   1, "xor_rm",        &ISel::build_bin_rm, Want::Reg, Slot(Op::Load, 1), Want::Addr).with(Cond::Fold),
    rule(Op::BitNand,   Ty::Reg,   1, "nand",          &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNand,   Ty::Reg,   1, "nand_imm",      &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitNor,    Ty::Reg,   1, "nor",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNor,    Ty::Reg,   1, "nor_imm",       &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::BitNxor,   Ty::Reg,   1, "nxor",          &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::BitNxor,   Ty::Reg,   1, "nxor_imm",      &ISel::build_bin, Want::Reg, Want::Imm),
    rule(Op::ShrL,      Ty::Reg,   1, "shr",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::ShrL,      Ty::Reg,   1, "shr_imm",       &ISel::build_bin, Want::Reg, Want::Sh),
    rule(Op::ShrA,      Ty::Reg,   1, "sar",           &ISel::build_bin, Want::Reg, Want::Reg),
    rule(Op::ShrA,      Ty::Reg,   1, "sar_imm",       &ISel::build_bin, Want::Reg, Want::Sh),

    rule(Op::Icmp,      Ty::Reg,   2, "cmp",           &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Icmp,      Ty::Reg,   2, "cmp_imm",       &ISel::build_cmp, Want::Reg, Want::Imm),
    rule(Op::Ncmp,      Ty::Reg,   2, "ncmp",          &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Ncmp,      Ty::Reg,   2, "ncmp_imm",      &ISel::build_cmp, Want::Reg, Want::Imm),

    rule(Op::NatAdd,    Ty::Reg,   1, "nat_add",       &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatAdd,    Ty::Reg,   1, "nat_add_imm",   &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatSub,    Ty::Reg,   1, "nat_sub",       &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatSub,    Ty::Reg,   1, "nat_sub_imm",   &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatMul,    Ty::Reg,   1, "nat_mul",       &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatMul,    Ty::Reg,   1, "nat_mul_imm",   &ISel::build_nat, Want::Reg, Want::Imm),
    rule(Op::NatDiv,    Ty::Reg,   1, "nat_div",       &ISel::build_nat, Want::Reg, Want::Reg),
    rule(Op::NatRem,    Ty::Reg,   1, "nat_rem",       &ISel::build_nat, Want::Reg, Want::Reg),

    rule(Op::Conv,      Ty::Reg,   1, "conv",          &ISel::build_unary, Want::Reg),
    rule(Op::Idx,       Ty::Reg,   1, "idx",           &ISel::build_unary, Want::Reg),
    rule(Op::Idx2Nat,   Ty::Reg,   1, "idx2nat",       &ISel::build_unary, Want::Reg),
    rule(Op::Bitcast,   Ty::Reg,   1, "bitcast",       &ISel::build_unary, Want::Reg),
    rule(Op::Rem,       Ty::Reg,   1, "rem",           &ISel::build_unary, Want::Reg),

    rule(Op::Div,       Ty::Reg,   1, "div",           &ISel::build_div, Want::Reg, Want::Reg),
    rule(Op::Slot,      Ty::Reg,   1, "slot",          &ISel::build_slot),
    rule(Op::Assume,    Ty::Reg,   0, "assume",        &ISel::build_assume),
    rule(Op::Select,    Ty::Reg,   1, "select",        &ISel::build_select, Want::Reg, Want::Reg, Want::Reg),
};
// clang-format on

static_assert(std::ranges::all_of(rules_, well_formed));

constexpr bool has_atomic(Op op) {
    return std::ranges::any_of(rules_, [op](const Rule& r) { return r.op == op && atomic(r); });
}

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
    auto& w = new_world();
    auto a  = hole(alt.demands[0].def);
    auto b  = hole(alt.demands[1].def, alt.demands[1].want);
    return w.call(*cc(root), bin<cmp>(w, a, b));
}

const Def* ISel::build_nat(const Def* root, const Alt& alt) {
    auto& w = new_world();
    auto a  = hole(alt.demands[0].def);
    auto b  = hole(alt.demands[1].def, alt.demands[1].want);
    switch (Axm::as<core::nat>(root).id()) {
        case core::nat::add: return bin<add>(w, a, b);
        case core::nat::mul: return bin<imul>(w, a, b);
        case core::nat::sub:
            return w.call(x86::cmovcc::b, Defs{bin<cmp>(w, a, b), bin<sub>(w, a, b), reg(w, w.lit_i64(0))});
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
    auto code = x86::cmovcc::ne;
    auto fl   = rr<test>(w, c, c);
    if (auto cond = Axm::isa<x86::cc>(c)) {
        code = cmovcc_of(cond.sub());
        fl   = cond->arg();
    }
    // `cmov` has no 8-bit form.
    if (reg_width(size) < 16) {
        auto ext = [&](const Def* v) { return reg(w, zext(w.lit_i32(), v)); };
        return w.call<trunc>(size, w.call(code, Defs{fl, ext(x), ext(y)}));
    }
    return w.call(code, Defs{fl, x, y});
}

std::optional<ISel::Node> ISel::decompose(const Def* def) const {
    if (auto ex = def->isa<Extract>()) {
        auto index = ex->index();
        if (xsize(ex->type()) && Idx::isa_lit(index->type()) == 2 && !Lit::isa(index)
            && ex->tuple()->num_projs() == 2) {
            auto [x, y] = ex->tuple()->projs<2>();
            return Node{Op::Select, {x, y, index}};
        }
        return {};
    }

    auto app = def->isa<App>();
    if (!app) return {};
    auto ptrn = pattern(app);
    if (!ptrn) return {};
    auto node = [&](DefVec ops) { return Node{ptrn->op, std::move(ops), ptrn->commutes}; };

    switch (ptrn->tag) {
        case Tag::Lea: {
            auto [base, i] = app->args<2>();
            if (offset(app)) return node({base});
            auto size = elem_size(app);
            return Node{size == 1 || size == 2 || size == 4 || size == 8 ? Op::LeaScale : Op::LeaMul, {base, i}};
        }
        case Tag::Load:
            if (!xsize(app->type()->proj(2, 1))) return {};
            return node({app->arg(2, 1)});
        case Tag::Store: {
            auto [_, p, x] = app->args<3>();
            if (!xsize(x->type())) return {};
            return node({p, x});
        }
        case Tag::Cmp:
            if (!cc(app)) return {};
            [[fallthrough]];
        case Tag::Bin: {
            auto [a, b] = app->args<2>();
            return node({a, b});
        }
        case Tag::Unary: return node({app->arg()});
        case Tag::Div: {
            auto [x, y] = app->arg(2, 1)->projs<2>();
            return node({x, y});
        }
        case Tag::None: return node({});
    }
    std::unreachable();
}

void ISel::match(const Def* def, Alts& alts) const {
    if (auto node = decompose(def))
        for (const auto& rule : rules_)
            if (rule.op == node->op) append(rule, def, *node, alts);
}

void ISel::append(const Rule& rule, const Def* root, const Node& node, Alts& alts) const {
    auto alt = Alt();
    alt.rule = &rule;
    alt.cost = rule.cost;
    if (node.commutes && node.ops.size() == 2 && !symmetric(rule, 0)) {
        bind(rule, root, 0, alt, {Frame{root, node.ops}}, alts);
        bind(rule, root, 0, std::move(alt), {Frame{root, {node.ops[1], node.ops[0]}}}, alts);
    } else {
        bind(rule, root, 0, std::move(alt), {Frame{root, node.ops}}, alts);
    }
}

void ISel::bind(const Rule& rule, const Def* root, size_t i, Alt alt, fe::Vector<Frame> frames, Alts& alts) const {
    while (!frames.empty() && frames.back().next == frames.back().ops.size())
        frames.pop_back();
    if (frames.empty()) {
        if (i == rule.num_slots && holds(root, alt)) alts.emplace_back(std::move(alt));
        return;
    }
    if (i == rule.num_slots) return;

    auto& f       = frames.back();
    auto def      = f.ops[f.next++];
    auto slot     = rule.slots[i];
    auto& demands = f.cover == size_t(-1) ? alt.demands : alt.covers[f.cover].demands;
    if (slot.is_hole()) {
        if ((slot.want == Want::Imm || slot.want == Want::Sh) && !literal(def, slot.want)) return;
        demands.emplace_back(Demand{def, slot.want});
        return bind(rule, root, i + 1, std::move(alt), std::move(frames), alts);
    }

    auto app  = sole(f, def);
    auto node = app ? decompose(app) : std::nullopt;
    if (!node || node->ops.size() != slot.arity) return;
    if (slot.node == Op::Alu ? !has_mem_form(*pattern(app), app, true) : node->op != slot.node) return;
    auto c = alt.covers.size();
    demands.emplace_back(Demand{def, Want::Reg, c + 1});
    alt.covers.emplace_back(Cover{app, 0, {}});
    auto frame = Frame{app, std::move(node->ops), 0, c};
    if (node->commutes && frame.ops.size() == 2 && !symmetric(rule, i + 1)) {
        auto swapped = frames;
        swapped.emplace_back(Frame{app, {frame.ops[1], frame.ops[0]}, 0, c});
        frames.emplace_back(std::move(frame));
        bind(rule, root, i + 1, alt, std::move(frames), alts);
        bind(rule, root, i + 1, std::move(alt), std::move(swapped), alts);
    } else {
        frames.emplace_back(std::move(frame));
        bind(rule, root, i + 1, std::move(alt), std::move(frames), alts);
    }
}

const App* ISel::sole(const Frame& parent, const Def* def) const {
    if (auto app = parent.node->isa<App>(); !app || uses(app->arg()) != 1) return nullptr;
    if (std::ranges::count(parent.ops, def) != 1) return nullptr;
    if (key(def) != def) return sole_load(def);
    return uses(def) == 1 ? def->isa<App>() : nullptr;
}

bool ISel::holds(const Def* root, const Alt& alt) const {
    switch (alt.rule->cond) {
        case Cond::None: return true;
        case Cond::Fold: {
            if (!has_mem_form(*pattern(root), root, false)) return false;
            // The replacement sits at the load, so the other operands must be available there.
            auto ld = alt.covers.back().def->as<App>();
            auto bb = chain(ld->arg(2, 0));
            if (!bb) return false;
            for (const auto& d : alt.demands) {
                auto done = DefSet();
                if (!d.inner && (!d.def->in_scope(bb) || depends(d.def, ld, done))) return false;
            }
            return true;
        }
        case Cond::Rmw: {
            // The load's memory has no other user, so nothing lies between load and store.
            auto [m, p, _] = root->as<App>()->args<3>();
            auto ld        = alt.covers.back().def->as<App>();
            return ld->arg(2, 1) == p && ld->proj(2, 0) == m && uses(m) == 1;
        }
    }
    std::unreachable();
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
    // A cover's operands follow it in preorder, so their classes are known before its own.
    auto resolve = [](const Alt& alt, fe::Vector<Demand>& demands) {
        for (auto& d : demands)
            if (d.inner) d.inner = alt.covers[d.inner - 1].inner;
    };
    for (auto def : nodes)
        for (auto& alt : alts_[def]) {
            for (size_t j = alt.covers.size(); j-- != 0;) {
                auto& cover = alt.covers[j];
                resolve(alt, cover.demands);
                auto& inner = alts_.at(cover.def);
                auto same   = [&](const Alt& a) { return !a.rule && std::ranges::equal(a.demands, cover.demands); };
                if (auto i = std::ranges::find_if(inner, same); i != inner.end()) {
                    cover.inner = i->inner;
                } else {
                    auto a      = Alt();
                    a.inner     = inner.size() + 1;
                    a.demands   = cover.demands;
                    cover.inner = a.inner;
                    inner.emplace_back(std::move(a));
                }
            }
            resolve(alt, alt.demands);
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
        for (size_t i = 0; i != r.num_slots; ++i) {
            auto s = r.slots[i];
            if (!s.is_hole()) {
                if (s.node != Op::Alu && !has_atomic(s.node))
                    fe::throwf("x86 isel: `{}` folds `{}`, but no atomic rule selects it", r.name, op_name(s.node));
                for (const auto& p : patterns_)
                    if (s.node == Op::Alu && p.insn && !has_atomic(p.op))
                        fe::throwf("x86 isel: `{}` folds `{}`, but no atomic rule selects it", r.name, op_name(p.op));
                continue;
            }
            if (s.want == Want::Imm || s.want == Want::Sh) continue; // satisfied by a literal, not a rule type
            bool ok = false;
            for (auto ty : {Ty::Reg, Ty::Base, Ty::Index})
                if (!conv(ty, s.want).is_inf()) ok = true;
            if (!ok) fe::throwf("x86 isel: no rule type converts to a hole of `{}`", r.name);
        }
    }

    for (const auto& r : rules_)
        if (!has_atomic(r.op)) fe::throwf("x86 isel: no atomic rule for `{}`", op_name(r.op));
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
