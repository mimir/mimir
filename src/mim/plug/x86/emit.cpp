#include "mim/plug/x86/emit.h"

#include <bit>
#include <map>
#include <print>
#include <ranges>

#include <mim/plugin.h>

#include <mim/plug/mach/emit.h>
#include <mim/plug/math/math.h>

#include "mim/plug/x86/insn.h"
#include "mim/plug/x86/target.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

using namespace std::string_literals;
using mach::Reg;
using mach::Src;

namespace {

using Names = std::array<std::string_view, 4>;

// clang-format off
constexpr std::array<Names, 16> names = {{
    {"%al",   "%ax",   "%eax",  "%rax"}, {"%cl",   "%cx",   "%ecx",  "%rcx"},
    {"%dl",   "%dx",   "%edx",  "%rdx"}, {"%bl",   "%bx",   "%ebx",  "%rbx"},
    {"%spl",  "%sp",   "%esp",  "%rsp"}, {"%bpl",  "%bp",   "%ebp",  "%rbp"},
    {"%sil",  "%si",   "%esi",  "%rsi"}, {"%dil",  "%di",   "%edi",  "%rdi"},
    {"%r8b",  "%r8w",  "%r8d",  "%r8" }, {"%r9b",  "%r9w",  "%r9d",  "%r9" },
    {"%r10b", "%r10w", "%r10d", "%r10"}, {"%r11b", "%r11w", "%r11d", "%r11"},
    {"%r12b", "%r12w", "%r12d", "%r12"}, {"%r13b", "%r13w", "%r13d", "%r13"},
    {"%r14b", "%r14w", "%r14d", "%r14"}, {"%r15b", "%r15w", "%r15d", "%r15"},
}};
// clang-format on

constexpr std::array<std::string_view, 16> ccs
    = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};

/// Per `x86.fcmp` subtag: the `setcc` after `ucomiss`, and the result of an unordered comparison where `setcc` errs.
/// An unordered `ucomiss` sets ZF, PF, and CF; isel swaps operands so that only `e` and `une` need the fixup.
constexpr std::array<std::pair<std::string_view, std::optional<bool>>, 16> fccs = {{
    // clang-format off
    {"", {}},  {"e", false}, {"b", false}, {"be", false}, {"a", {}},   {"ae", {}},   {"ne", {}},   {"np", {}},
    {"p", {}}, {"e", {}},    {"b", {}},    {"be", {}},    {"a", true}, {"ae", true}, {"ne", true}, {"", {}},
    // clang-format on
}};
constexpr size_t Fcmp_T                                                         = 15;

size_t log_bytes(nat_t w) { return std::countr_zero(w / 8); }
std::string_view name(size_t r, nat_t w) { return names[r][log_bytes(w)]; }
char suffix(nat_t w) { return "bwlq"[log_bytes(w)]; }
std::string xmm(size_t r) { return std::format("%xmm{}", r); }
/// The single/double suffix of a floating-point mnemonic.
std::string_view fsz(nat_t w) { return w == 64 ? "sd" : "ss"; }

/// Mnemonic of a floating-point arithmetic instruction without its single/double suffix.
std::string_view farith(flags_t id) {
    if (id == Annex::Base<fadd>) return "add";
    if (id == Annex::Base<fsub>) return "sub";
    if (id == Annex::Base<fmul>) return "mul";
    return "div";
}

/// Mnemonic of a two-address ALU instruction.
std::string_view alu(flags_t id) {
    // clang-format off
    if (id == Annex::Base<add>)  return "add";
    if (id == Annex::Base<sub>)  return "sub";
    if (id == Annex::Base<and_>) return "and";
    if (id == Annex::Base<or_>)  return "or";
    if (id == Annex::Base<xor_>) return "xor";
    if (id == Annex::Base<imul>) return "imul";
    // clang-format on
    return {};
}

/// Width of the register holding a value of @p type; a spill slot is as wide as its value.
nat_t width(const Def* type) {
    if (auto w = math::isa_f(type)) return *w;
    if (auto spill = Axm::isa<mach::Spill>(type)) return width(spill->arg());
    return std::bit_ceil(std::max<nat_t>(Idx::expect_bitwidth(type, "an integer"), 8));
}

/// Writes one function in AT&T syntax.
class Emitter : public mach::Emitter {
public:
    Emitter(std::ostream& os, Emit& emit, Lam* fun)
        : mach::Emitter(x86::target(), os, emit, fun)
        , emit_(emit) {}

private:
    std::string slot(size_t i, size_t off = 0) const {
        return std::format("{}(%rbp)", int64_t(off) - int64_t(8 * (saved(Target::Gpr).size() + i + 1)));
    }
    /// An operand that is a register or an immediate.
    std::string src(const Def* def, nat_t w) const;
    std::string imm(const Def* lit, nat_t w) const;
    std::string addr(const Def*) const;
    /// A 16-byte constant in the `.rodata` pool, by its two halves.
    std::string pool(u64 lo, u64 hi);
    /// The `andps`/`andpd` mask that clears the sign bit of a value @p w bits wide.
    std::string abs_mask(nat_t w) {
        return w == 64 ? pool(0x7fff'ffff'ffff'ffff, 0x7fff'ffff'ffff'ffff)
                       : pool(0x7fff'ffff'7fff'ffff, 0x7fff'ffff'7fff'ffff);
    }
    /// The `f64` 2^32, which turns a signed `i32` into an unsigned one.
    std::string two32() { return pool(std::bit_cast<u64>(4294967296.0), 0); }
    /// A fresh label for an instruction's own fixup.
    std::string local(std::string_view what) {
        return std::format("{}{}.{}{}", target().local_prefix(), fun()->unique_name(), what, sel_++);
    }
    /// `ucomiss src, a` and the `setcc` of the `x86.fcmp` subtag @p sub into @p dst.
    void ucomi(size_t sub, nat_t w, std::string_view src, size_t a, size_t dst);
    /// The `x86.fsqrt` or conversion @p id of the operand @p src, @p ws bits wide, into the result @p res.
    void unary(flags_t id, std::string_view src, nat_t ws, const Def* res);
    void insn(const Def* res, const Def* f, const Def* x);

    void step(const App*) final;
    void move(Reg dst, Reg src) final;
    /// Swaps the contents of @p a and @p b; the XMM registers have no `xchg`.
    void swap(Reg a, Reg b) final;
    void mov_imm(Reg dst, const Def* lit) final;
    void push_slot(size_t i) final { line("pushq {}", slot(i)); }
    void pop_slot(size_t i) final { line("popq {}", slot(i)); }
    void spill(Src src, const Def* type, size_t i) final;
    void reload(Reg dst, const Def* type, size_t i) final;
    /// The area grows down from `rbp`, so its address is that of its last slot.
    void slot_addr(size_t dst, size_t first, size_t n) final {
        line("leaq {}, {}", slot(first + n - 1), name(dst, 64));
    }
    void jmp(std::string_view label) final { line("jmp {}", label); }
    void branch_cc(size_t cc, std::string_view label) final { line("j{} {}", ccs[cc], label); }
    void branch_if(size_t r, std::string_view label) final {
        auto n = name(r, 8);
        line("testb {}, {}", n, n);
        line("jne {}", label);
    }
    void call_insn(std::string_view sym, const std::array<size_t, Target::Num_Classes>& num_args) final {
        // A variadic callee reads the number of vector registers used from `al`.
        line("movl ${}, %eax", num_args[Target::Fpr]);
        line("call {}", sym);
    }
    void prologue(std::ostream& os) final;
    void epilogue() final;

    Emit& emit_;
    size_t sel_ = 0;
};

std::string Emitter::imm(const Def* lit, nat_t w) const {
    auto l = *Lit::isa(lit);
    auto s = std::bit_cast<int64_t>(l << (64 - w)) >> (64 - w);
    if (s < INT32_MIN || s > INT32_MAX)
        lit->blame("{} backend: literal `{}` does not fit an immediate", backend(), lit).bail();
    return std::format("${}", s);
}

std::string Emitter::src(const Def* def, nat_t w) const {
    if (Lit::isa(def)) return imm(def, w);
    return std::string(name(use_reg(def), w));
}

std::string Emitter::addr(const Def* a) const {
    auto ix           = Axm::isa<x86::index>(a);
    auto [disp, base] = Axm::as<x86::addr>(ix ? ix->decurry()->arg() : a)->args<2>();
    auto d            = std::bit_cast<int32_t>(u32(nat(disp)));
    if (auto l = Lit::isa(base); l && *l != 0) base->blame("{} backend: literal base `{}`", backend(), base).bail();
    auto has_base = !Lit::isa(base);
    if (!has_base && !ix) return std::to_string(d);

    auto res = d != 0 ? std::to_string(d) : ""s;
    res += "(";
    if (has_base) res += name(use_reg(base), 64);
    if (ix) {
        auto [index, scale] = ix->args<2>();
        res += std::format(",{},{}", name(use_reg(index), 64), 1 << nat(scale));
    }
    return res + ")";
}

std::string Emitter::pool(u64 lo, u64 hi) {
    return std::format("{}pool{}(%rip)", target().local_prefix(), emit_.pool(lo, hi));
}

void Emitter::move(Reg dst, Reg src) {
    if (dst.cls == Target::Fpr)
        line("movaps {}, {}", xmm(src.num), xmm(dst.num));
    else
        line("movq {}, {}", name(src.num, 64), name(dst.num, 64));
}

/// Swaps the contents of @p a and @p b; the XMM registers have no `xchg`.
void Emitter::swap(Reg a, Reg b) {
    if (a.cls == Target::Gpr) return line("xchgq {}, {}", name(a.num, 64), name(b.num, 64));
    line("xorps {}, {}", xmm(a.num), xmm(b.num));
    line("xorps {}, {}", xmm(b.num), xmm(a.num));
    line("xorps {}, {}", xmm(a.num), xmm(b.num));
}

void Emitter::mov_imm(Reg dst, const Def* lit) {
    auto w = width(lit->type());
    auto l = *Lit::isa(lit);
    if (dst.cls == Target::Fpr) {
        if (w != 32 && w != 64) lit->blame("{} backend: `math.F` of {} bits is not supported", backend(), w).bail();
        return line("mov{} {}, {}", fsz(w), pool(l, w == 64 ? 0 : l), xmm(dst.num));
    }
    // `xorl` clobbers the flags, so only use it while none are pending.
    if (l == 0 && flags_clobbered()) return line("xorl {}, {}", name(dst.num, 32), name(dst.num, 32));
    if (w < 64) return line("movl ${}, {}", l, name(dst.num, 32));
    auto s = std::bit_cast<int64_t>(l);
    if (s < INT32_MIN || s > INT32_MAX)
        line("movabsq ${}, {}", s, name(dst.num, 64));
    else
        line("movq ${}, {}", s, name(dst.num, 64));
}

void Emitter::insn(const Def* res, const Def* f, const Def* x) {
    auto axm  = std::get<0>(Axm::get(f));
    auto id   = axm->base();
    auto keep = id == Annex::Base<mov> || id == Annex::Base<lea> || id == Annex::Base<movzx> || id == Annex::Base<movsx>
             || id == Annex::Base<cc> || id == Annex::Base<cmovcc> || id == Annex::Base<fmov> || id == Annex::Base<fadd>
             || id == Annex::Base<fsub> || id == Annex::Base<fmul> || id == Annex::Base<fdiv>
             || id == Annex::Base<fsqrt> || id == Annex::Base<fabs> || id == Annex::Base<i2f> || id == Annex::Base<f2i>
             || id == Annex::Base<f2u> || id == Annex::Base<f2f>;
    if (!keep) clobber_flags();

    auto binary = [&](std::string_view mnemonic) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        auto dst    = def_reg(res);
        assert(use_reg(a) == dst);
        line("{}{} {}, {}", mnemonic, suffix(w), src(b, w), name(dst, w));
    };
    auto shift = [&](std::string_view mnemonic) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        auto dst    = def_reg(res);
        assert(use_reg(a) == dst);
        if (auto l = Lit::isa(b))
            line("{}{} ${}, {}", mnemonic, suffix(w), *l % w, name(dst, w));
        else
            line("{}{} %cl, {}", mnemonic, suffix(w), name(dst, w));
    };
    auto divide = [&](bool sign) {
        auto [a, b] = x->projs<2>();
        auto w      = width(a->type());
        assert(use_reg(a) == encoding(reg::rax));
        if (!sign)
            line("xorl %edx, %edx");
        else
            line("{}", w == 64 ? "cqto" : w == 32 ? "cltd" : "cwtd");
        line("{}div{} {}", sign ? "i" : "", suffix(w), name(use_reg(b), w));
    };
    auto extend = [&](char kind) {
        auto ws = width(x->type());
        auto wd = width(res->type());
        auto rs = use_reg(x);
        auto rd = def_reg(res);
        if (ws < wd && !(kind == 'z' && ws == 32))
            line("mov{}{}{} {}, {}", kind, suffix(ws), suffix(wd), name(rs, ws), name(rd, wd));
        else if (ws < wd)
            line("movl {}, {}", name(rs, 32), name(rd, 32));
        else if (rs != rd)
            line("movq {}, {}", name(rs, 64), name(rd, 64));
    };

    // clang-format off
    if      (auto mn = alu(id); !mn.empty()) binary(mn);
    else if (id == Annex::Base<div>)   divide(false);
    else if (id == Annex::Base<idiv>)  divide(true);
    else if (id == Annex::Base<shl>)   shift("shl");
    else if (id == Annex::Base<shr>)   shift("shr");
    else if (id == Annex::Base<sar>)   shift("sar");
    else if (id == Annex::Base<movzx>) extend('z');
    else if (id == Annex::Base<movsx>) extend('s');
    // clang-format on
    else if (id == Annex::Base<mov>) {
        if (!Lit::isa(x)) x->blame("{} backend: `x86.mov` of `{}`, which is no literal", backend(), x).bail();
        mov_imm(Reg{def_reg(res), Target::Gpr}, x);
    } else if (id == Annex::Base<lea>) {
        line("leaq {}, {}", addr(x), name(def_reg(res), 64));
    } else if (id == Annex::Base<trunc>) {
        auto w    = width(res->type());
        auto rs   = use_reg(x);
        auto rd   = def_reg(res);
        auto n    = *Idx::isa_lit(res->type());
        auto full = w == 64 ? 0 : 1_n << w;
        if (n != 0 && !std::has_single_bit(n)) {
            // Nothing reserves rax, rcx, and rdx for the division, so save them around it.
            auto save = fe::Vector<size_t>();
            for (auto r : {encoding(reg::rax), encoding(reg::rcx), encoding(reg::rdx)})
                if (r != rd) save.emplace_back(r);
            for (auto r : save)
                line("pushq {}", name(r, 64));
            auto wx = width(x->type());
            auto ws = std::max<nat_t>(wx, 32);
            if (wx < 32)
                line("movz{}l {}, %eax", suffix(wx), name(rs, wx));
            else
                line("mov{} {}, {}", suffix(ws), name(rs, ws), name(encoding(reg::rax), ws));
            line("xorl %edx, %edx");
            line("mov{} ${}, {}", suffix(ws), n, name(encoding(reg::rcx), ws));
            line("div{} {}", suffix(ws), name(encoding(reg::rcx), ws));
            if (rd != encoding(reg::rdx)) line("movq %rdx, {}", name(rd, 64));
            for (auto r : save | std::views::reverse)
                line("popq {}", name(r, 64));
        } else {
            if (rs != rd) line("movq {}, {}", name(rs, 64), name(rd, 64));
            if (std::has_single_bit(n) && n != full) line("and{} ${}, {}", suffix(w), n - 1, name(rd, w));
        }
    } else if (id == Annex::Base<cmp> || id == Annex::Base<test>) {
        auto [a, b] = x->projs<2>();
        auto w      = width(a->type());
        line("{}{} {}, {}", id == Annex::Base<cmp> ? "cmp" : "test", suffix(w), src(b, w), name(use_reg(a), w));
        set_flags(res);
    } else if (id == Annex::Base<cc>) {
        use_flags(x);
        // A condition that only a branch consumes has no register.
        if (auto r = isa_reg(res)) line("set{} {}", ccs[axm->sub()], name(*r, 8));
    } else if (id == Annex::Base<cmovcc>) {
        auto [fl, a, b] = x->projs<3>();
        auto w          = width(res->type());
        auto dst        = def_reg(res);
        assert(use_reg(a) == dst);
        use_flags(fl);
        line("cmov{} {}, {}", ccs[axm->sub()], name(use_reg(b), w), name(dst, w));
    } else if (id == Annex::Base<fadd> || id == Annex::Base<fsub> || id == Annex::Base<fmul>
               || id == Annex::Base<fdiv>) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        auto dst    = def_reg(res);
        assert(use_reg(a) == dst);
        line("{}{} {}, {}", farith(id), fsz(w), xmm(use_reg(b)), xmm(dst));
    } else if (id == Annex::Base<fsqrt> || id == Annex::Base<i2f> || id == Annex::Base<f2i> || id == Annex::Base<f2u>
               || id == Annex::Base<f2f>) {
        auto ws = width(x->type());
        auto r  = use_reg(x);
        unary(id, math::isa_f(x->type()) ? xmm(r) : std::string(name(r, ws)), ws, res);
    } else if (id == Annex::Base<fabs>) {
        auto w   = width(res->type());
        auto dst = def_reg(res);
        assert(use_reg(x) == dst);
        line("and{} {}, {}", w == 64 ? "pd" : "ps", abs_mask(w), xmm(dst));
    } else if (id == Annex::Base<fmov>) {
        if (!Lit::isa(x)) x->blame("{} backend: `x86.fmov` of `{}`, which is no literal", backend(), x).bail();
        mov_imm(Reg{def_reg(res), Target::Fpr}, x);
    } else if (id == Annex::Base<fcmp>) {
        auto [a, b] = x->projs<2>();
        ucomi(axm->sub(), width(a->type()), xmm(use_reg(b)), use_reg(a), def_reg(res));
    } else if (id == Annex::Base<fsel>) {
        auto [c, a, b] = x->projs<3>();
        auto dst       = def_reg(res);
        assert(use_reg(a) == dst);
        auto l = local("fsel");
        line("testb {}, {}", name(use_reg(c), 8), name(use_reg(c), 8));
        line("je {}", l);
        line("movaps {}, {}", xmm(use_reg(b)), xmm(dst));
        label_here(l);
    } else if (id == Annex::Base<u2f>) {
        auto ws = width(x->type());
        auto wr = width(res->type());
        if (ws == 64) f->blame("{} backend: `x86.u2f` from `i64` is not supported yet", backend()).bail();
        auto src = name(use_reg(x), 32);
        auto dst = xmm(def_reg(res));
        auto l   = local("u2f");
        // Exact in `f64`, so an `f32` result rounds only once.
        line("cvtsi2sdl {}, {}", src, dst);
        line("testl {}, {}", src, src);
        line("jns {}", l);
        line("addsd {}, {}", two32(), dst);
        label_here(l);
        if (wr == 32) line("cvtsd2ss {}, {}", dst, dst);
    } else {
        f->blame("{} backend: cannot emit `{}`", backend(), axm).bail();
    }
}

void Emitter::ucomi(size_t sub, nat_t w, std::string_view src, size_t a, size_t dst) {
    auto [cc, unordered] = fccs[sub];
    if (cc.empty()) return line("movl ${}, {}", sub == Fcmp_T ? 1 : 0, name(dst, 32));
    line("ucomi{} {}, {}", fsz(w), src, xmm(a));
    line("set{} {}", cc, name(dst, 8));
    if (!unordered) return;
    auto l = local("fcmp");
    line("jnp {}", l);
    line("movl ${}, {}", *unordered ? 1 : 0, name(dst, 32));
    label_here(l);
}

void Emitter::unary(flags_t id, std::string_view src, nat_t ws, const Def* res) {
    auto wr  = width(res->type());
    auto dst = def_reg(res);
    if (id == Annex::Base<fsqrt>) return line("sqrt{} {}, {}", fsz(wr), src, xmm(dst));
    if (id == Annex::Base<i2f>) return line("cvtsi2{}{} {}, {}", fsz(wr), ws == 64 ? "q" : "l", src, xmm(dst));
    if (id == Annex::Base<f2i>) return line("cvtt{}2si{} {}, {}", fsz(ws), wr == 64 ? "q" : "l", src, name(dst, wr));
    if (id == Annex::Base<f2f>) return line("cvt{}2{} {}, {}", fsz(ws), fsz(wr), src, xmm(dst));
    if (wr == 64) res->blame("{} backend: `x86.f2u` to `i64` is not supported yet", backend()).bail();
    line("cvtt{}2siq {}, {}", fsz(ws), src, name(dst, 64));
    line("movl {}, {}", name(dst, 32), name(dst, 32));
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), Target::sched_insn(app), app->arg());

    if (!Axm::isa<load>(app) && !Axm::isa<store>(app) && !Axm::isa<fload>(app) && !Axm::isa<fstore>(app))
        clobber_flags();

    if (auto op = Axm::isa<rm>(app)) {
        auto [_, a, p] = app->args<3>();
        auto w         = width(a->type());
        auto dst       = def_reg(app->proj(2, 1));
        auto insn      = op->decurry()->arg()->as<App>()->axm();
        if (insn->base() == Annex::Base<fcmp>) return ucomi(insn->sub(), w, addr(p), use_reg(a), dst);
        assert(use_reg(a) == dst);
        if (math::isa_f(a->type()))
            line("{}{} {}, {}", farith(insn->base()), fsz(w), addr(p), xmm(dst));
        else
            line("{}{} {}, {}", alu(insn->flags()), suffix(w), addr(p), name(dst, w));
        return;
    }

    if (Axm::isa<rm1>(app)) {
        auto [_, p] = app->args<2>();
        auto f      = app->callee()->as<App>()->arg();
        return unary(insn::op(app), addr(p), width(f->type()->as<Pi>()->dom()), app->proj(2, 1));
    }

    if (auto op = Axm::isa<mr>(app)) {
        auto [_, p, b] = app->args<3>();
        auto w         = width(b->type());
        line("{}{} {}, {}", alu(op->decurry()->arg()->as<App>()->axm()->flags()), suffix(w), src(b, w), addr(p));
        return;
    }

    if (Axm::isa<load>(app)) {
        auto [_, p] = app->args<2>();
        auto res    = app->proj(2, 1);
        auto w      = width(res->type());
        line("mov{} {}, {}", suffix(w), addr(p), name(def_reg(res), w));
        return;
    }

    if (Axm::isa<fload>(app)) {
        auto [_, p] = app->args<2>();
        auto res    = app->proj(2, 1);
        auto w      = width(res->type());
        line("mov{} {}, {}", fsz(w), addr(p), xmm(def_reg(res)));
        return;
    }

    if (Axm::isa<fstore>(app)) {
        auto [_, p, v] = app->args<3>();
        auto w         = width(v->type());
        line("mov{} {}, {}", fsz(w), xmm(use_reg(v)), addr(p));
        return;
    }

    auto [_, p, v] = app->args<3>();
    auto w         = width(v->type());
    line("mov{} {}, {}", suffix(w), src(v, w), addr(p));
}

void Emitter::spill(Src src, const Def* type, size_t i) {
    if (src.reg.cls != Target::Fpr) {
        auto s = src.imm ? imm(src.imm, 64) : std::string(name(src.reg.num, 64));
        return line("movq {}, {}", s, slot(i));
    }
    auto w = width(type);
    if (src.imm) {
        auto l = *Lit::isa(src.imm);
        line("movl ${}, {}", u32(l), slot(i));
        if (w == 64) line("movl ${}, {}", u32(l >> 32), slot(i, 4));
    } else {
        line("mov{} {}, {}", fsz(w), xmm(src.reg.num), slot(i));
    }
}

void Emitter::reload(Reg dst, const Def* type, size_t i) {
    if (dst.cls == Target::Fpr)
        line("mov{} {}, {}", fsz(width(type)), slot(i), xmm(dst.num));
    else
        line("movq {}, {}", slot(i), name(dst.num, 64));
}

void Emitter::epilogue() {
    if (saved(Target::Gpr).empty()) {
        line("leave");
    } else {
        line("leaq -{}(%rbp), %rsp", 8 * saved(Target::Gpr).size());
        for (auto r : saved(Target::Gpr) | std::views::reverse)
            line("popq {}", name(r, 64));
        line("popq %rbp");
    }
    line("ret");
}

void Emitter::prologue(std::ostream& os) {
    auto frame = frame_size() - 8 * saved(Target::Gpr).size();
    std::println(os, "\tpushq %rbp");
    std::println(os, "\tmovq %rsp, %rbp");
    for (auto r : saved(Target::Gpr))
        std::println(os, "\tpushq {}", name(r, 64));
    if (frame != 0) std::println(os, "\tsubq ${}, %rsp", frame);
}

} // namespace

void Emit::emit(std::ostream& os, Lam* fun, bool global) { Emitter(os, *this, fun).emit(global); }

void Emit::finish(std::ostream& os) {
    if (pool_.empty()) return;
    auto constants = fe::Vector<std::pair<u64, u64>>(pool_.size());
    for (auto& [c, i] : pool_)
        constants[i] = c;
    std::println(os, "\t{}", target().is_elf() ? ".section .rodata" : ".section __TEXT,__const");
    std::println(os, "\t.p2align 4");
    for (size_t i = 0; i != constants.size(); ++i) {
        std::println(os, "{}pool{}:", target().local_prefix(), i);
        std::println(os, "\t.quad 0x{:016x}", constants[i].first);
        std::println(os, "\t.quad 0x{:016x}", constants[i].second);
    }
}

} // namespace mim::plug::x86
