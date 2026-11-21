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
#include <mim/plug/refly/refly.h>

#include "mim/plug/x86/imm.h"
#include "mim/plug/x86/isel_rules.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

bool is_reg_size(nat_t size) { return size == 2 || size == core::widen_idx(size); }

/// @p def without the casts between pointer types around it, which `mem.legalize_ptr` inserts and which are no-ops.
const Def* strip(const Def* def) {
    while (auto cast = Axm::isa<core::bitcast>(def)) {
        if (!Axm::isa<core::Ptr>(def->type()) || !Axm::isa<core::Ptr>(cast->arg()->type())) break;
        def = cast->arg();
    }
    return def;
}

/// The `cmovcc` with the same subtag as @p code.
x86::cmovcc cmovcc_of(sub_t code) { return static_cast<x86::cmovcc>(Annex::Base<x86::cmovcc> | code); }

/// Does the old_world() or new_world() literal @p lit fit a sign-extended 32-bit immediate?
bool fits_imm(const Def* lit) { return x86::fits_imm(Idx::isa_lit(lit->type()), *Lit::isa(lit)); }

/// @p x in a register.
const Def* reg(World& w, const Def* x) { return Lit::isa(x) ? w.call<mov>(x) : x; }
/// @p x in a register or as an immediate.
const Def* ri(World& w, const Def* x) { return Lit::isa(x) && fits_imm(x) ? x : reg(w, x); }

/// `op r, r/imm`.
template<class Id>
const Def* bin(World& w, const Def* a, const Def* b) {
    return w.call<Id>(Defs{reg(w, a), ri(w, b)});
}

bool is_lowered_plugin(const Axm* axm) {
    auto p = axm->plugin();
    return p == core::Plugin_Id || p == mem::Plugin_Id || p == math::Plugin_Id;
}

} // namespace

/// How `match` extracts the operands of a pattern.
enum class Tag : u8 {
    Load,   ///< `mem.load (m, p)`.
    Store,  ///< `mem.store (m, p, x)`.
    Bin,    ///< Two operands.
    Unary,  ///< One operand.
    Div,    ///< Two operands behind the `divrem` tuple.
    None,   ///< No operands.
    Select, ///< `(x, y)#c`, an `Extract` rather than an axiom.
};

/// The x86 side of a pattern, for the builders that apply one instruction.
struct Insn {
    flags_t id = 0;     ///< The x86 instruction.
    s8 res     = -1;    ///< The result of a pair-valued instruction, or -1.
    bool sign  = false; ///< Operands narrower than any size the instruction supports are sign-extended.
};

/// A program-side operation `match` recognizes: its axiom, the `Op` it selects, how to take it apart, and its
/// instruction.
struct Pattern {
    flags_t flags;      ///< The axiom, or the whole annex if `member` is `false`.
    bool member = true; ///< @p flags names one member of the annex.
    Op op;              ///< The operation @p flags selects.
    Tag tag;            ///< How `match` extracts the operands.
    Insn insn     = {};
    bool commutes = true; ///< An immediate may also be the first operand.
};

namespace {

/// One member of a multi-member annex.
template<class Id>
constexpr Pattern member(Id id, Op op, Tag tag, Insn insn = {}, bool commutes = true) {
    return Pattern{flags_t(id), true, op, tag, insn, commutes};
}

/// The whole annex.
template<class Id>
constexpr Pattern annex(Op op, Tag tag, Insn insn = {}) {
    return Pattern{Annex::base<Id>(), false, op, tag, insn, false};
}

// clang-format off
/// The program side of the rule set: which `core`/`mem` axioms isel covers.
constexpr Pattern patterns_[] = {
    // The rules of `x86_isel` select these.
    member(core::wrap::add,  Op::WrapAdd, Tag::Bin),
    member(core::wrap::sub,  Op::WrapSub, Tag::Bin),
    member(core::wrap::mul,  Op::WrapMul, Tag::Bin),
    member(core::wrap::shl,  Op::WrapShl, Tag::Bin),
    member(core::bit2::and_, Op::BitAnd,  Tag::Bin),
    member(core::bit2::or_,  Op::BitOr,   Tag::Bin),
    member(core::bit2::xor_, Op::BitXor,  Tag::Bin),
    member(core::bit2::nand, Op::BitNand, Tag::Bin),
    member(core::bit2::nor,  Op::BitNor,  Tag::Bin),
    member(core::bit2::nxor, Op::BitNxor, Tag::Bin),
    member(core::shr::a,     Op::ShrA,    Tag::Bin),
    member(core::shr::l,     Op::ShrL,    Tag::Bin),
    // The members of an annex select different instructions.
    member(core::nat::add,   Op::NatAdd,  Tag::Bin,   {.id = Annex::Base<add>}),
    member(core::nat::sub,   Op::NatSub,  Tag::Bin,   {}, false),
    member(core::nat::mul,   Op::NatMul,  Tag::Bin,   {.id = Annex::Base<imul>}),
    member(core::nat::div,   Op::NatDiv,  Tag::Bin,   {.id = Annex::Base<div>,  .res = 0}, false),
    member(core::nat::rem,   Op::NatRem,  Tag::Bin,   {.id = Annex::Base<div>,  .res = 1}, false),
    member(core::div::udiv,  Op::Div,     Tag::Div,   {.id = Annex::Base<div>,  .res = 0}, false),
    member(core::div::urem,  Op::Div,     Tag::Div,   {.id = Annex::Base<div>,  .res = 1}, false),
    member(core::div::sdiv,  Op::Div,     Tag::Div,   {.id = Annex::Base<idiv>, .res = 0, .sign = true}, false),
    member(core::div::srem,  Op::Div,     Tag::Div,   {.id = Annex::Base<idiv>, .res = 1, .sign = true}, false),
    member(core::conv::s,    Op::Conv,    Tag::Unary, {.sign = true}, false),
    member(core::conv::u,    Op::Conv,    Tag::Unary, {}, false),

    member(core::icmp::e,    Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::e)},  false),
    member(core::icmp::ne,   Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::ne)}, false),
    member(core::icmp::ul,   Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::b)},  false),
    member(core::icmp::ule,  Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::be)}, false),
    member(core::icmp::ug,   Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::a)},  false),
    member(core::icmp::uge,  Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::ae)}, false),
    member(core::icmp::sl,   Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::l)},  false),
    member(core::icmp::sle,  Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::le)}, false),
    member(core::icmp::sg,   Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::g)},  false),
    member(core::icmp::sge,  Op::Icmp,    Tag::Bin,   {.id = flags_t(x86::cc::ge)}, false),
    member(core::ncmp::e,    Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::e)},  false),
    member(core::ncmp::ne,   Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::ne)}, false),
    member(core::ncmp::l,    Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::b)},  false),
    member(core::ncmp::le,   Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::be)}, false),
    member(core::ncmp::g,    Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::a)},  false),
    member(core::ncmp::ge,   Op::Ncmp,    Tag::Bin,   {.id = flags_t(x86::cc::ae)}, false),

    // One axiom per instruction.
    annex<core::idx>     (Op::Idx,       Tag::Unary),
    annex<core::idx2nat> (Op::Idx2Nat,   Tag::Unary),
    annex<core::bitcast> (Op::Bitcast,   Tag::Unary),
    annex<core::rem>     (Op::Rem,       Tag::Unary, {.id = Annex::Base<div>, .res = 1}),
    annex<mem::ptradd>   (Op::Ptradd,    Tag::Bin),
    annex<mem::load>     (Op::Load,      Tag::Load),
    annex<mem::store>    (Op::Store,     Tag::Store),
    annex<mem::mslot>    (Op::Slot,      Tag::None),
};

constexpr Pattern select_ = {.flags = 0, .member = false, .op = Op::Select, .tag = Tag::Select, .commutes = false};
// clang-format on

/// The condition of the first `refly.check` in @p def.
const Def* size_check(const Def* def, DefSet& done) {
    if (auto check = Axm::isa<refly::check>(def)) return check->arg(3, 0);
    if (!done.emplace(def).second) return nullptr;
    for (auto op : def->ops())
        if (auto cond = op ? size_check(op, done) : nullptr) return cond;
    return nullptr;
}

/// Is @p def a memory operation, whose value is its second result?
bool mem_valued(const Def* def) {
    auto t = def->type();
    return t->num_projs() == 2 && Axm::isa<core::M>(t->proj(2, 0));
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

/// The register size of the x86 `Idx` the old_world() @p type becomes.
std::optional<nat_t> new_size(const Def* type) {
    if (Axm::isa<core::Ptr>(type) || type->isa<Nat>()) return 0;
    if (auto size = Idx::isa(type)) return Lit::isa(size).value_or(0);
    return {};
}

/// The size of the x86 `Idx` the old_world() @p type becomes, if it fits a register.
std::optional<nat_t> xsize(const Def* type) {
    if (auto s = new_size(type); s && is_reg_size(*s)) return s;
    return {};
}

/// The pattern of @p def, or `nullptr` if isel does not cover it.
const Pattern* pattern(const Def* def) {
    if (strip(def) != def) return nullptr;
    if (def->isa<Extract>()) return &select_;
    auto [axm, curry, _] = Axm::get(def);
    if (!axm || curry) return nullptr;
    for (const auto& p : patterns_)
        if ((p.member ? axm->flags() == p.flags : axm->base() == p.flags)) return &p;
    return nullptr;
}

} // namespace

bool ISel::supports(flags_t insn, nat_t size) {
    if (auto res = supports_.find({insn, size}); res != supports_.end()) return res->second;
    auto& w   = new_world();
    auto pi   = w.annex(insn)->type()->as<Pi>();
    auto var  = pi->has_var();
    auto done = DefSet();
    // The `refly.check` in its codomain states the sizes it supports.
    auto cond                      = size_check(pi->codom(), done);
    return supports_[{insn, size}] = !var || !cond || VarRewriter(var, w.lit_nat(size)).rewrite(cond) == w.lit_tt();
}

const Def* ISel::call_insn(const Insn& insn, Want want, const Def* a, const Def* b) {
    auto& w   = new_world();
    auto call = [&](const Def* x, const Def* y) {
        auto r = w.call(insn.id, Defs{reg(w, x), is_literal(want) ? y : reg(w, y)});
        return insn.res < 0 ? r : r->proj(2, insn.res);
    };
    auto size = Idx::isa_lit(a->type());
    if (!size || supports(insn.id, core::widen_idx(*size))) return call(a, b);
    auto ext = [&](const Def* x) { return insn.sign ? w.call<movsx>(w.lit_i32(), reg(w, x)) : zext(w.lit_i32(), x); };
    return w.call<trunc>(*size, call(ext(a), ext(b)));
}

namespace {

/*
 * Costs
 */

constexpr pbqp::Cost Inf = pbqp::Cost::inf();

// clang-format off
/// Cost of converting a replacement or operand of type `Ty` (row) into a hole of type `Want` (column).
constexpr pbqp::Cost conv_[size_t(Ty::Num)][size_t(Want::Num)] = {
    //          Reg  Imm  Sh   Base Index Addr
    /* Reg   */ {0,  Inf, Inf, 0,   Inf,  0},
    /* Base  */ {1,  Inf, Inf, 0,   Inf,  0},
    /* Index */ {1,  Inf, Inf, 1,   0,    0},
    /* Lit   */ {1,  0,   0,   0,   Inf,  0},
};
// clang-format on

pbqp::Cost conv(Ty from, Want want) { return conv_[size_t(from)][size_t(want)]; }

/// Cost of the operand @p def, which is no node, in a hole of type @p want.
pbqp::Cost leaf(const Def* def, Want want) { return conv(Lit::isa(def) ? Ty::Lit : Ty::Reg, want); }

/// Does @p def satisfy a hole of type @p want that requires a literal?
bool literal(const Def* def, Want want) { return Lit::isa(def) && (want == Want::Sh || fits_imm(def)); }

/*
 * Rules
 */

template<class... S>
constexpr Rule rule(Op op, Ty type, pbqp::Cost cost, std::string_view name, BuildFn build, S... slots) {
    static_assert(sizeof...(S) <= Rule::Max_Slots);
    return Rule{op, type, cost, name, build, Cond::None, u8(sizeof...(S)), {slots...}};
}

/// An atomic rule has no inner nodes and only holes that every rule type can fill.
bool atomic(const Rule& r) {
    if (r.gen && r.gen->match->num_covers != 0) return false;
    for (size_t i = 0; i != r.num_slots; ++i)
        if (auto w = r.slots[i]; w != Want::Reg && w != Want::Base && w != Want::Addr) return false;
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
    rule(Op::Load,      Ty::Reg,   1, "load",          &ISel::build_load, Want::Addr),
    rule(Op::Store,     Ty::Reg,   1, "store",         &ISel::build_store, Want::Addr, Want::Reg),
    rule(Op::Store,     Ty::Reg,   1, "store_imm",     &ISel::build_store, Want::Addr, Want::Imm),

    rule(Op::Icmp,      Ty::Reg,   2, "cmp",           &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Icmp,      Ty::Reg,   2, "cmp_imm",       &ISel::build_cmp, Want::Reg, Want::Imm),
    rule(Op::Ncmp,      Ty::Reg,   2, "ncmp",          &ISel::build_cmp, Want::Reg, Want::Reg),
    rule(Op::Ncmp,      Ty::Reg,   2, "ncmp_imm",      &ISel::build_cmp, Want::Reg, Want::Imm),

    rule(Op::NatAdd,    Ty::Reg,   1, "nat_add",       &ISel::build_insn, Want::Reg, Want::Reg),
    rule(Op::NatAdd,    Ty::Reg,   1, "nat_add_imm",   &ISel::build_insn, Want::Reg, Want::Imm),
    rule(Op::NatSub,    Ty::Reg,   1, "nat_sub",       &ISel::build_nat_sub, Want::Reg, Want::Reg),
    rule(Op::NatSub,    Ty::Reg,   1, "nat_sub_imm",   &ISel::build_nat_sub, Want::Reg, Want::Imm),
    rule(Op::NatMul,    Ty::Reg,   1, "nat_mul",       &ISel::build_insn, Want::Reg, Want::Reg),
    rule(Op::NatMul,    Ty::Reg,   1, "nat_mul_imm",   &ISel::build_insn, Want::Reg, Want::Imm),
    rule(Op::NatDiv,    Ty::Reg,   1, "nat_div",       &ISel::build_insn, Want::Reg, Want::Reg),
    rule(Op::NatRem,    Ty::Reg,   1, "nat_rem",       &ISel::build_insn, Want::Reg, Want::Reg),

    rule(Op::Conv,      Ty::Reg,   0, "conv_id",       &ISel::build_id, Want::Reg).with(Cond::Same),
    rule(Op::Conv,      Ty::Reg,   1, "conv_trunc",    &ISel::build_trunc, Want::Reg).with(Cond::Narrow),
    rule(Op::Conv,      Ty::Reg,   1, "conv_ext",      &ISel::build_ext, Want::Reg).with(Cond::Widen),
    rule(Op::Idx,       Ty::Reg,   0, "idx",           &ISel::build_id, Want::Reg).with(Cond::Same),
    rule(Op::Idx2Nat,   Ty::Reg,   0, "idx2nat_id",    &ISel::build_id, Want::Reg).with(Cond::Same),
    rule(Op::Idx2Nat,   Ty::Reg,   1, "idx2nat_ext",   &ISel::build_ext, Want::Reg).with(Cond::Widen),
    rule(Op::Bitcast,   Ty::Reg,   0, "bitcast_id",    &ISel::build_id, Want::Reg).with(Cond::Same),
    rule(Op::Bitcast,   Ty::Reg,   1, "bitcast_trunc", &ISel::build_trunc, Want::Reg).with(Cond::Narrow),
    rule(Op::Bitcast,   Ty::Reg,   1, "bitcast_ext",   &ISel::build_ext, Want::Reg).with(Cond::Widen),
    rule(Op::Rem,       Ty::Reg,   1, "rem",           &ISel::build_rem, Want::Reg),

    rule(Op::Div,       Ty::Reg,   1, "div",           &ISel::build_insn, Want::Reg, Want::Reg),
    rule(Op::Slot,      Ty::Reg,   1, "slot",          &ISel::build_slot),
    rule(Op::Select,    Ty::Reg,   1, "select",        &ISel::build_select, Want::Reg, Want::Reg, Want::Reg),
};
// clang-format on

/*
 * Generated Rules
 */

const rules::Want& want_of(const rules::Rewrite& r, size_t i) { return rules::wants[r.wants + i]; }

Want want(rules::Want w) {
    switch (w) {
        case rules::Want::Imm: return Want::Imm;
        case rules::Want::Sh: return Want::Sh;
        case rules::Want::Base: return Want::Base;
        case rules::Want::Index: return Want::Index;
        case rules::Want::Addr: return Want::Addr;
        default: return Want::Reg;
    }
}

std::vector<Rule> make_generated() {
    auto res = std::vector<Rule>();
    for (const auto& r : rules::rewrites) {
        auto p = std::ranges::find_if(patterns_, [&r](const Pattern& p) {
            return p.member ? p.flags == r.root : p.flags == Annex::flags2base(r.root);
        });
        if (p == std::end(patterns_)) fe::throwf("x86 isel: no pattern for the root of rule `{}`", r.name);
        auto type = r.head == Annex::Base<x86::addr> ? Ty::Base : r.head == Annex::Base<index> ? Ty::Index : Ty::Reg;
        auto rule = Rule{.op    = p->op,
                         .type  = type,
                         .cost  = r.cost,
                         .name  = r.name,
                         .build = &ISel::build_gen,
                         .cond  = r.maps ? Cond::Fold : Cond::None,
                         .gen   = &r};
        for (size_t i = 0; i != r.num_metas; ++i)
            if (auto w = want_of(r, i); w != rules::Want::Param && rule.num_slots != Rule::Max_Slots)
                rule.slots[rule.num_slots++] = want(w);
        res.emplace_back(rule);
    }
    return res;
}

/// The rules of `x86_isel`.
const std::vector<Rule>& generated() {
    static const auto rules = make_generated();
    return rules;
}

#ifdef MIM_ENABLE_CHECKS
bool has_atomic(Op op) {
    auto is = [op](const Rule& r) { return r.op == op && atomic(r); };
    return std::ranges::any_of(rules_, is) || std::ranges::any_of(generated(), is);
}
#endif

} // namespace

void ISel::append_gen(const Rule& rule, const Def* root, Alts& alts) {
    auto swaps = rule.gen->swaps;
    // Ascending subsets of `swaps`.
    for (u32 swap = 0;; swap = ((swap | ~swaps) + 1) & swaps) {
        append_gen(rule, root, swap, alts);
        if (swap == swaps) break;
    }
}

void ISel::append_gen(const Rule& rule, const Def* root, u32 swap, Alts& alts) {
    const auto& r = *rule.gen;
    const auto& m = *r.match;
    auto metas    = DefVec(r.num_metas, [](size_t) { return nullptr; });
    auto refs     = DefVec(m.num_refs, [](size_t) { return nullptr; });
    if (!m.fn(root, swap, metas.data(), refs.data())) return;

    auto alt = Alt();
    alt.rule = &rule;
    alt.cost = rule.cost;
    alt.covers.resize(m.num_covers, Cover{nullptr, 0, {}, false});
    for (size_t j = 0; j != m.num_refs; ++j) {
        auto info                  = rules::all_refs[m.refs + j];
        auto ex                    = refs[j]->isa<Extract>();
        auto cover                 = ex ? ex->tuple() : refs[j];
        alt.covers[info.cover].def = cover;
        if (info.kind == rules::Ref::Mem && uses(refs[j]) != 1) return;
    }

    // A pure inner node that something else uses is looked through: the match takes its operands but not the node.
    auto parent = std::vector<u8>(m.num_covers, 0);
    auto owner  = [&](u8 o) {
        while (o != 0 && alt.covers[o - 1].through)
            o = parent[o - 1];
        return o;
    };
    auto node    = [&](u8 o) { return o == 0 ? root : alt.covers[o - 1].def; };
    auto demands = [&](u8 o) -> auto& { return o == 0 ? alt.demands : alt.covers[o - 1].demands; };
    for (size_t j = 0; j != m.num_uses; ++j) {
        auto u = rules::all_uses[m.uses + j];
        auto o = owner(u.owner);
        if (u.ref) {
            auto def  = refs[u.index];
            auto c    = rules::all_refs[m.refs + u.index].cover;
            auto& cov = alt.covers[c];
            parent[c] = o;
            if (!decompose(cov.def)) return;
            if (sole(node(u.owner), def) != cov.def) {
                if (mem_valued(cov.def) || c + 1u == r.maps) return;
                cov.through = true;
                continue;
            }
            demands(o).emplace_back(Demand{def, Want::Reg, size_t(c) + 1});
        } else if (auto w = want_of(r, u.index); w != rules::Want::Param) {
            auto def = metas[u.index];
            if (is_literal(want(w)) && !literal(def, want(w))) return;
            demands(o).emplace_back(Demand{def, want(w)});
        }
    }
    if (!holds(root, alt)) return;
    alt.metas = std::move(metas);
    alts.emplace_back(std::move(alt));
}

const Def* ISel::build_gen(const Def*, const Alt& alt) {
    const auto& r = *alt.rule->gen;
    auto holes    = DefVec(r.num_metas, [&](size_t i) {
        auto w = want_of(r, i);
        return w == rules::Want::Param ? rewrite(alt.metas[i]) : hole(alt.metas[i], want(w));
    });
    auto res      = r.build(new_world(), holes.data());
    if (!r.maps) return res;
    map(alt.covers[r.maps - 1].def, res);
    return res->proj(2, 1);
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

const Def* ISel::build_insn(const Def* root, const Alt& alt) {
    auto& w    = new_world();
    auto& insn = pattern(root)->insn;
    auto a     = hole(alt.demands[0].def, alt.demands[0].want);
    auto b     = hole(alt.demands[1].def, alt.demands[1].want);
    auto res   = call_insn(insn, alt.demands[1].want, a, b);
    return mem_valued(root) ? w.tuple({rewrite(root->as<App>()->arg(2, 0)), res}) : res;
}

const Def* ISel::build_cmp(const Def* root, const Alt& alt) {
    auto& w = new_world();
    auto a  = hole(alt.demands[0].def);
    auto b  = hole(alt.demands[1].def, alt.demands[1].want);
    return w.call(pattern(root)->insn.id, bin<cmp>(w, a, b));
}

const Def* ISel::build_nat_sub(const Def*, const Alt& alt) {
    auto& w = new_world();
    auto a  = hole(alt.demands[0].def);
    auto b  = hole(alt.demands[1].def, alt.demands[1].want);
    return w.call(x86::cmovcc::b, Defs{bin<cmp>(w, a, b), bin<sub>(w, a, b), reg(w, w.lit_i64(0))});
}

const Def* ISel::build_id(const Def*, const Alt& alt) { return hole(alt.demands[0].def); }

const Def* ISel::build_trunc(const Def* root, const Alt& alt) {
    return new_world().call<trunc>(*Idx::isa_lit(rewrite(root->type())), hole(alt.demands[0].def));
}

const Def* ISel::build_ext(const Def* root, const Alt& alt) {
    auto& w  = new_world();
    auto dst = *Idx::isa_lit(rewrite(root->type()));
    auto x   = hole(alt.demands[0].def);
    return pattern(root)->insn.sign ? w.call<movsx>(dst, x) : w.call<movzx>(dst, x);
}

const Def* ISel::build_rem(const Def* root, const Alt& alt) {
    auto& insn = pattern(root)->insn;
    auto x     = hole(alt.demands[0].def);
    auto d     = new_world().lit(x->type(), *Lit::isa(Axm::as<core::rem>(root)->decurry()->arg()));
    return call_insn(insn, Want::Reg, x, d);
}

const Def* ISel::build_slot(const Def* root, const Alt&) {
    auto& w        = new_world();
    auto [msz, k]  = Axm::as<mem::mslot>(root)->args<2>();
    auto [m, size] = msz->projs<2>();
    return w.app(rewrite(k), w.call<slot>(*Lit::isa(size), rewrite(m)));
}

const Def* ISel::build_select(const Def* root, const Alt& alt) {
    auto& w   = new_world();
    auto size = *xsize(root->type());
    auto x    = hole(alt.demands[0].def);
    auto y    = hole(alt.demands[1].def);
    auto c    = hole(alt.demands[2].def);
    auto code = x86::cmovcc::ne;
    auto fl   = w.call<test>(Defs{reg(w, c), reg(w, c)});
    if (auto cond = Axm::isa<x86::cc>(c)) {
        code = cmovcc_of(cond.sub());
        fl   = cond->arg();
    }
    if (!supports(flags_t(code), size)) {
        auto ext = [&](const Def* v) { return reg(w, zext(w.lit_i32(), v)); };
        return w.call<trunc>(size, w.call(code, Defs{fl, ext(x), ext(y)}));
    }
    return w.call(code, Defs{fl, x, y});
}

std::optional<ISel::Node> ISel::decompose(const Def* def) const {
    auto ptrn = pattern(def);
    if (!ptrn) return {};
    auto app  = def->isa<App>();
    auto node = [&](DefVec ops) { return Node{ptrn, ptrn->op, std::move(ops)}; };

    switch (ptrn->tag) {
        case Tag::Select: {
            auto ex    = def->as<Extract>();
            auto index = ex->index();
            if (!xsize(ex->type()) || Idx::isa_lit(index->type()) != 2 || Lit::isa(index)
                || ex->tuple()->num_projs() != 2)
                return {};
            auto [x, y] = ex->tuple()->projs<2>();
            return node({x, y, index});
        }
        case Tag::Load:
            if (!xsize(app->type()->proj(2, 1))) return {};
            return node({app->arg(2, 1)});
        case Tag::Store: {
            auto [_, p, x] = app->args<3>();
            if (!xsize(x->type())) return {};
            return node({p, x});
        }
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

void ISel::match(const Def* def, Alts& alts) {
    auto node = decompose(def);
    if (!node) return;
    if (auto axm = std::get<0>(Axm::get(def)))
        for (const auto& rule : generated())
            if (rule.gen->root == axm->flags()) append_gen(rule, def, alts);
    for (const auto& rule : rules_)
        if (rule.op == node->op) append(rule, def, *node, alts);
}

void ISel::append(const Rule& rule, const Def* root, const Node& node, Alts& alts) {
    if (node.ops.size() != rule.num_slots) return;
    auto swaps = node.ptrn->commutes && rule.num_slots == 2 && rule.slots[0] != rule.slots[1];
    for (bool swap : {false, true}) {
        if (swap && !swaps) break;
        auto alt = Alt();
        alt.rule = &rule;
        alt.cost = rule.cost;
        auto ok  = true;
        for (size_t i = 0; i != rule.num_slots && ok; ++i) {
            auto def  = node.ops[swap ? 1 - i : i];
            auto want = rule.slots[i];
            ok        = !is_literal(want) || literal(def, want);
            alt.demands.emplace_back(Demand{def, want});
        }
        if (ok && holds(root, alt)) alts.emplace_back(std::move(alt));
    }
}

const App* ISel::sole(const Def* parent, const Def* def) const {
    auto ops = decompose(parent);
    if (auto app = parent->isa<App>(); !app || !ops || uses(app->arg()) != 1) return nullptr;
    if (std::ranges::count(ops->ops, def) != 1) return nullptr;
    if (def->isa<Extract>() && key(def) != def) return sole_value(def);
    return uses(def) == 1 ? def->isa<App>() : nullptr;
}

bool ISel::holds(const Def* root, const Alt& alt) {
    switch (alt.rule->cond) {
        case Cond::None: return true;
        case Cond::Same:
        case Cond::Narrow:
        case Cond::Widen: {
            auto src = new_size(alt.demands[0].def->type());
            auto dst = new_size(root->type());
            if (!src || !dst) return false;
            if (*src == *dst) return alt.rule->cond == Cond::Same;
            return alt.rule->cond == (reg_width(*src) >= reg_width(*dst) ? Cond::Narrow : Cond::Widen);
        }
        case Cond::Fold: {
            auto ld = alt.covers[alt.rule->gen->maps - 1].def->as<App>();
            auto bb = chain(ld->arg(2, 0));
            if (!bb) return false;
            for (const auto& d : alt.demands) {
                auto done = DefSet();
                if (!d.inner && (!d.def->in_scope(bb) || depends(d.def, ld, done))) return false;
            }
            return true;
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
                if (cover.through) continue;
                resolve(alt, cover.demands);
                auto& inner = alts_.at(cover.def);
                auto same   = [&](const Alt& a) { return !a.rule && std::ranges::equal(a.demands, cover.demands); };
                auto i      = std::ranges::find_if(inner, same);
                cover.inner = size_t(i - inner.begin()) + 1;
                if (i == inner.end()) {
                    auto a    = Alt();
                    a.demands = cover.demands;
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
                        c += !o.rule && j + 1 == d.inner ? 0 : pbqp::Cost::inf();
                    else
                        c += o.rule ? conv(o.rule->type, d.want) : pbqp::Cost::inf();
                }
                return c;
            });
            if (std::ranges::any_of(costs, [](pbqp::Cost c) { return c != 0; }))
                graph.add_edge_costs(ids[def], ids[op], costs);
        }
    }

    auto fallback = pbqp::Choices(graph.num_nodes(), 0);
    for (auto def : nodes) {
        auto i             = std::ranges::find_if(alts_[def], [](const Alt& a) { return a.rule && atomic(*a.rule); });
        auto found         = i != alts_[def].end();
        fallback[ids[def]] = found ? size_t(i - alts_[def].begin()) : 0;
#ifdef MIM_ENABLE_CHECKS
        if (!found) fe::throwf("x86 isel: `{}` has no atomic rule", def);
#endif
    }
#ifdef MIM_ENABLE_CHECKS
    if (pbqp::evaluate(graph, fallback).is_inf()) fe::throwf("the atomic rules must cover the program");
#endif
    auto sol = pbqp::solve(graph);
    log().d("x86 isel: {} nodes, {} edges, cost {}", graph.num_nodes(), graph.num_edges(), sol.cost.value());
    // RN is a heuristic, but the atomic rules always remain a valid cover.
    const auto& picked = sol.feasible() ? sol.choices : fallback;
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
            if (!cover.through) roots_[cover.def] = def;
    }
    return false;
}

#ifdef MIM_ENABLE_CHECKS
void ISel::check_rules() const {
    auto all = std::vector<Rule>(std::begin(rules_), std::end(rules_));
    all.insert(all.end(), generated().begin(), generated().end());
    for (const auto& r : all) {
        if (conv(r.type, Want::Reg).is_inf())
            fe::throwf("x86 isel: rule `{}` yields a value that does not convert to a register", r.name);
        for (size_t i = 0; i != r.num_slots; ++i) {
            auto w = r.slots[i];
            if (is_literal(w)) continue; // satisfied by a literal, not a rule type
            bool ok = false;
            for (auto ty : {Ty::Reg, Ty::Base, Ty::Index})
                if (!conv(ty, w).is_inf()) ok = true;
            if (!ok) fe::throwf("x86 isel: no rule type converts to a hole of `{}`", r.name);
        }
    }

    for (const auto& r : all)
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

const App* ISel::sole_value(const Def* def) const {
    if (auto app = key(def)->isa<App>(); app != def && uses(def) == 1 && uses(app) == 2)
        if (auto m = app->proj(2, 0); m && uses(m) != 0) return app;
    return nullptr;
}

const Def* ISel::key(const Def* def) const {
    def = strip(def);
    if (auto ex = def->isa<Extract>(); ex && Lit::isa(ex->index()) == 1)
        if (auto app = ex->tuple()->isa<App>(); app && pattern(app) && mem_valued(app)) return app;
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
    if (auto alt = choice(strip(def)); alt && alt->rule && alt->rule->type != Ty::Reg)
        return convert(select(strip(def)), alt->rule->type, want);
    return convert(val(def), Ty::Reg, want);
}

const Def* ISel::convert(const Def* x, Ty from, Want want) {
    auto& w = new_world();
    if (from == Ty::Reg) return want == Want::Reg ? reg(w, x) : is_literal(want) ? x : addr({x});
    auto a = split(x);
    if (want == Want::Reg || is_literal(want)) return reg(w, materialize(a));
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
    if (auto ptr = strip(app); ptr != app) return rewrite(ptr);
    // Loop hints for LLVM.
    if (Axm::isa<ll::vec>(app) || Axm::isa<ll::novec>(app)) return rewrite(app->arg());

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !app->type()->isa<Type>()
                               && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
