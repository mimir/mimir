#include "mim/plug/x86/emit.h"

#include <bit>
#include <deque>
#include <map>
#include <print>
#include <ranges>
#include <sstream>

#include <mim/plugin.h>

#include <mim/plug/math/math.h>

#include "mim/plug/x86/constraint.h"
#include "mim/plug/x86/insn.h"
#include "mim/plug/x86/liveness.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

using namespace std::string_literals;

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
    if (auto spill = Axm::isa<Spill>(type)) type = type->world().type_idx(spill->arg());
    return std::bit_ceil(std::max<nat_t>(Idx::expect_bitwidth(type, "an integer"), 8));
}

nat_t nat(const Def* def) {
    if (auto l = Lit::isa(def)) return *l;
    def->blame(MIM_X86_BE "expected a literal, got `{}`", def).bail();
}

/// Functions still to emit, shared by all Emitter%s.
struct Funs {
    std::deque<Lam*> todo;
    LamSet seen;
    /// 16-byte constants of the `.rodata` pool; a label is allocated on first use.
    std::map<std::pair<u64, u64>, size_t> pool;

    void push(Lam* lam) {
        if (seen.emplace(lam).second) todo.emplace_back(lam);
    }
};

/// A physical register of one of the two register classes.
struct Reg {
    size_t num;
    Liveness::Class cls;
    bool operator==(const Reg&) const = default;
};

/// The source of a copy: a register or a literal.
struct Src {
    Reg reg        = {};
    const Def* imm = nullptr;
};

using Moves = fe::Vector<std::pair<Reg, Src>>;

/// Does a jump to @p lam pass values, which a branch cannot do on its own?
bool takes_values(Lam* lam) {
    return std::ranges::any_of(lam->vars(), [](const Def* x) { return !is_mem(x); });
}

class Emitter {
public:
    Emitter(std::ostream& os, Funs& funs, Lam* fun)
        : os_(os)
        , funs_(funs)
        , fun_(fun)
        , ret_(fun->ret_var())
        , live_(fun) {}

    void emit(bool global);

private:
    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::print(body_, "\t");
        std::println(body_, fmt, std::forward<Args>(args)...);
    }

    std::string slot(size_t i, size_t off = 0) const {
        return std::format("{}(%rbp)", int64_t(off) - int64_t(8 * (saved_.size() + i + 1)));
    }
    static std::string label(Lam* lam) { return std::format(".L{}", lam->unique_name()); }
    std::string symbol(Lam*);
    void collect(const Def*, DefSet&);
    void epilogue();

    size_t slot_of(const Def*);
    /// The register of the result @p def.
    size_t def_reg(const Def* def) const;
    /// The register of the operand @p def, an `x86.in r v` or `x86.fin r v`.
    size_t use_reg(const Def* def) const;
    /// An operand that is a register or an immediate.
    std::string src(const Def* def, nat_t w) const;
    Src source(const Def* def) const {
        return Lit::isa(def) ? Src{.imm = def} : Src{.reg = {use_reg(def), Liveness::cls(strip(def))}};
    }
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
    std::string local(std::string_view what) { return std::format(".L{}.{}{}", fun_->unique_name(), what, sel_++); }
    void move(Reg dst, Reg src);
    void swap(Reg a, Reg b);
    void mov_imm(Reg dst, const Def* lit);
    /// `ucomiss src, a` and the `setcc` of the `x86.fcmp` subtag @p sub into @p dst.
    void ucomi(size_t sub, nat_t w, std::string_view src, size_t a, size_t dst);
    /// The `x86.fsqrt` or conversion @p id of the operand @p src, @p ws bits wide, into the result @p res.
    void unary(flags_t id, std::string_view src, nat_t ws, const Def* res);
    void use_flags(const Def* fl) const;

    void insn(const Def* res, const Def* f, const Def* x);
    void step(const App*);
    void block(const Liveness::Block&);
    Lam* tail(const Liveness::Block&) const;
    void parallel(Moves moves, fe::Vector<std::pair<size_t, size_t>> slot_moves);
    void copy(Lam* target, const Def* arg);
    void jump(Lam* target);
    void call(Lam* callee, const Def* arg);

    std::ostream& os_;
    Funs& funs_;
    Lam* fun_;
    const Def* ret_;
    Liveness live_;
    std::ostringstream body_;
    DefMap<size_t> regs_;
    DefMap<size_t> slots_;
    size_t num_slots_ = 0;
    fe::Vector<size_t> saved_;
    Lam* next_        = nullptr;
    const Def* flags_ = nullptr; ///< The result of the `cmp` or `test` that the flags hold.
    size_t sel_       = 0;
};

std::string Emitter::symbol(Lam* lam) {
    if (lam->is_external() || !lam->is_set()) return lam->sym().str();
    funs_.push(lam);
    return lam->unique_name();
}

void Emitter::collect(const Def* def, DefSet& done) {
    if (def->isa_mut() || def->is_closed() || is_mem(def) || !done.emplace(def).second) return;
    if (Axm::isa<x86::in>(def) || Axm::isa<x86::fin>(def)) {
        regs_.emplace(strip(def), in_reg(def));
        return;
    }
    for (auto op : def->ops())
        collect(op, done);
}

size_t Emitter::slot_of(const Def* def) {
    auto [i, ins] = slots_.emplace(def, num_slots_);
    if (ins) ++num_slots_;
    return i->second;
}

size_t Emitter::def_reg(const Def* def) const {
    if (auto r = fe::lookup(regs_, def)) return *r;
    def->blame(MIM_X86_BE "`{}` has no register", def).bail();
}

size_t Emitter::use_reg(const Def* def) const {
    if (Axm::isa<x86::in>(def) || Axm::isa<x86::fin>(def)) return in_reg(def);
    if (Lit::isa(def)) def->blame(MIM_X86_BE "literal `{}` needs an `x86.mov` here", def).bail();
    def->blame(MIM_X86_BE "`{}` is not in a register", def).bail();
}

std::string Emitter::imm(const Def* lit, nat_t w) const {
    auto l = *Lit::isa(lit);
    auto s = std::bit_cast<int64_t>(l << (64 - w)) >> (64 - w);
    if (s < INT32_MIN || s > INT32_MAX) lit->blame(MIM_X86_BE "literal `{}` does not fit an immediate", lit).bail();
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
    if (auto l = Lit::isa(base); l && *l != 0) base->blame(MIM_X86_BE "literal base `{}`", base).bail();
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

void Emitter::move(Reg dst, Reg src) {
    if (dst.cls == Liveness::Fpr)
        line("movaps {}, {}", xmm(src.num), xmm(dst.num));
    else
        line("movq {}, {}", name(src.num, 64), name(dst.num, 64));
}

/// Swaps the contents of @p a and @p b; the XMM registers have no `xchg`.
void Emitter::swap(Reg a, Reg b) {
    if (a.cls == Liveness::Gpr) return line("xchgq {}, {}", name(a.num, 64), name(b.num, 64));
    line("xorps {}, {}", xmm(a.num), xmm(b.num));
    line("xorps {}, {}", xmm(b.num), xmm(a.num));
    line("xorps {}, {}", xmm(a.num), xmm(b.num));
}

void Emitter::mov_imm(Reg dst, const Def* lit) {
    auto w = width(lit->type());
    auto l = *Lit::isa(lit);
    if (dst.cls == Liveness::Fpr) {
        if (w != 32 && w != 64) lit->blame(MIM_X86_BE "`math.F` of {} bits is not supported", w).bail();
        return line("mov{} {}, {}", fsz(w), pool(l, w == 64 ? 0 : l), xmm(dst.num));
    }
    // `xorl` clobbers the flags, so only use it while none are pending.
    if (l == 0 && flags_ == nullptr) return line("xorl {}, {}", name(dst.num, 32), name(dst.num, 32));
    if (w < 64) return line("movl ${}, {}", l, name(dst.num, 32));
    auto s = std::bit_cast<int64_t>(l);
    if (s < INT32_MIN || s > INT32_MAX)
        line("movabsq ${}, {}", s, name(dst.num, 64));
    else
        line("movq ${}, {}", s, name(dst.num, 64));
}

std::string Emitter::pool(u64 lo, u64 hi) {
    auto [i, _] = funs_.pool.emplace(std::pair(lo, hi), funs_.pool.size());
    return std::format(".Lpool{}(%rip)", i->second);
}

void Emitter::use_flags(const Def* fl) const {
    if (fl != flags_) fl->blame(MIM_X86_BE "flags `{}` are clobbered before their use", fl).bail();
}

void Emitter::insn(const Def* res, const Def* f, const Def* x) {
    auto axm  = std::get<0>(Axm::get(f));
    auto id   = axm->base();
    auto keep = id == Annex::Base<mov> || id == Annex::Base<x86::spill> || id == Annex::Base<reload>
             || id == Annex::Base<lea> || id == Annex::Base<movzx> || id == Annex::Base<movsx> || id == Annex::Base<cc>
             || id == Annex::Base<cmovcc> || id == Annex::Base<fmov> || id == Annex::Base<fspill>
             || id == Annex::Base<freload> || id == Annex::Base<fadd> || id == Annex::Base<fsub>
             || id == Annex::Base<fmul> || id == Annex::Base<fdiv> || id == Annex::Base<fsqrt>
             || id == Annex::Base<fabs> || id == Annex::Base<i2f> || id == Annex::Base<f2i> || id == Annex::Base<f2u>
             || id == Annex::Base<f2f>;
    if (!keep) flags_ = nullptr;

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
        if (!Lit::isa(x)) x->blame(MIM_X86_BE "`x86.mov` of `{}`, which is no literal", x).bail();
        mov_imm(Reg{def_reg(res), Liveness::Gpr}, x);
    } else if (id == Annex::Base<x86::spill>) {
        if (Lit::isa(x))
            line("movq {}, {}", imm(x, 64), slot(slot_of(res)));
        else
            line("movq {}, {}", name(use_reg(x), 64), slot(slot_of(res)));
    } else if (id == Annex::Base<reload>) {
        line("movq {}, {}", slot(slot_of(x)), name(def_reg(res), 64));
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
        flags_ = res;
    } else if (id == Annex::Base<cc>) {
        use_flags(x);
        // A condition that only a branch consumes has no register.
        if (auto r = fe::lookup(regs_, res)) line("set{} {}", ccs[axm->sub()], name(*r, 8));
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
        if (!Lit::isa(x)) x->blame(MIM_X86_BE "`x86.fmov` of `{}`, which is no literal", x).bail();
        mov_imm(Reg{def_reg(res), Liveness::Fpr}, x);
    } else if (id == Annex::Base<fspill>) {
        auto w = width(x->type());
        auto s = slot_of(res);
        if (auto l = Lit::isa(x)) {
            line("movl ${}, {}", u32(*l), slot(s));
            if (w == 64) line("movl ${}, {}", u32(*l >> 32), slot(s, 4));
        } else {
            line("mov{} {}, {}", fsz(w), xmm(use_reg(x)), slot(s));
        }
    } else if (id == Annex::Base<freload>) {
        auto w = width(res->type());
        line("mov{} {}, {}", fsz(w), slot(slot_of(x)), xmm(def_reg(res)));
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
        std::println(body_, "{}:", l);
    } else if (id == Annex::Base<u2f>) {
        auto ws = width(x->type());
        auto wr = width(res->type());
        if (ws == 64) f->blame(MIM_X86_BE "`x86.u2f` from `i64` is not supported yet").bail();
        auto src = name(use_reg(x), 32);
        auto dst = xmm(def_reg(res));
        auto l   = local("u2f");
        // Exact in `f64`, so an `f32` result rounds only once.
        line("cvtsi2sdl {}, {}", src, dst);
        line("testl {}, {}", src, src);
        line("jns {}", l);
        line("addsd {}, {}", two32(), dst);
        std::println(body_, "{}:", l);
        if (wr == 32) line("cvtsd2ss {}, {}", dst, dst);
    } else {
        f->blame(MIM_X86_BE "cannot emit `{}`", axm).bail();
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
    std::println(body_, "{}:", l);
}

void Emitter::unary(flags_t id, std::string_view src, nat_t ws, const Def* res) {
    auto wr  = width(res->type());
    auto dst = def_reg(res);
    if (id == Annex::Base<fsqrt>) return line("sqrt{} {}, {}", fsz(wr), src, xmm(dst));
    if (id == Annex::Base<i2f>) return line("cvtsi2{}{} {}, {}", fsz(wr), ws == 64 ? "q" : "l", src, xmm(dst));
    if (id == Annex::Base<f2i>) return line("cvtt{}2si{} {}, {}", fsz(ws), wr == 64 ? "q" : "l", src, name(dst, wr));
    if (id == Annex::Base<f2f>) return line("cvt{}2{} {}, {}", fsz(ws), fsz(wr), src, xmm(dst));
    if (wr == 64) res->blame(MIM_X86_BE "`x86.f2u` to `i64` is not supported yet").bail();
    line("cvtt{}2siq {}, {}", fsz(ws), src, name(dst, 64));
    line("movl {}, {}", name(dst, 32), name(dst, 32));
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), sched_insn(app), app->arg());

    if (Axm::isa<x86::slot>(app)) {
        num_slots_ += (nat(app->callee()->as<App>()->arg()) + 7) / 8;
        line("leaq {}, {}", slot(num_slots_ - 1), name(def_reg(app->proj(2, 1)), 64));
        return;
    }

    if (!Axm::isa<load>(app) && !Axm::isa<store>(app) && !Axm::isa<fload>(app) && !Axm::isa<fstore>(app))
        flags_ = nullptr;

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

/// Sequentializes the parallel copies @p moves between registers and @p slot_moves between spill slots.
void Emitter::parallel(Moves moves, fe::Vector<std::pair<size_t, size_t>> slot_moves) {
    std::erase_if(slot_moves, [](auto m) { return m.first == m.second; });
    for (auto [_, from] : slot_moves)
        line("pushq {}", slot(from));
    for (auto [to, _] : slot_moves | std::views::reverse)
        line("popq {}", slot(to));

    auto imms = Moves();
    for (auto& m : moves)
        if (m.second.imm) imms.emplace_back(m);
    std::erase_if(moves, [](const auto& m) { return m.second.imm || m.first == m.second.reg; });

    while (!moves.empty()) {
        auto free = std::ranges::find_if(moves, [&](const auto& m) {
            return std::ranges::none_of(moves, [&](const auto& n) { return n.second.reg == m.first; });
        });
        if (free != moves.end()) {
            move(free->first, free->second.reg);
            moves.erase(free);
            continue;
        }
        // Only cycles are left.
        auto [d, s] = moves.back();
        moves.pop_back();
        swap(s.reg, d);
        for (auto& [dst, from] : moves)
            if (from.reg == d)
                from.reg = s.reg;
            else if (from.reg == s.reg)
                from.reg = d;
        std::erase_if(moves, [](const auto& m) { return m.first == m.second.reg; });
    }

    for (auto [dst, s] : imms)
        mov_imm(dst, s.imm);
}

void Emitter::copy(Lam* target, const Def* arg) {
    auto moves      = Moves();
    auto slot_moves = fe::Vector<std::pair<size_t, size_t>>();
    auto n          = target->num_vars();
    for (size_t i = 0; i != n; ++i) {
        auto x = target->var(n, i);
        auto a = arg->proj(n, i);
        if (is_mem(x)) continue;
        if (Axm::isa<Spill>(x->type()) || Axm::isa<FSpill>(x->type()))
            slot_moves.emplace_back(slot_of(x), slot_of(a));
        else if (auto r = fe::lookup(regs_, x))
            moves.emplace_back(Reg{*r, Liveness::cls(x)}, source(a));
    }
    parallel(std::move(moves), std::move(slot_moves));
}

void Emitter::jump(Lam* target) {
    if (target != next_) line("jmp {}", label(target));
}

void Emitter::epilogue() {
    if (saved_.empty()) {
        line("leave");
    } else {
        line("leaq -{}(%rbp), %rsp", 8 * saved_.size());
        for (auto r : saved_ | std::views::reverse)
            line("popq {}", name(r, 64));
        line("popq %rbp");
    }
    line("ret");
}

void Emitter::call(Lam* callee, const Def* arg) {
    auto n     = arg->num_projs();
    auto moves = Moves();
    auto a = size_t(0), f = size_t(0);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (auto x = arg->proj(n, i); !is_mem(x)) {
            auto cls = Liveness::cls(strip(x));
            auto& c  = cls == Liveness::Gpr ? a : f;
            if (c == num_arg_regs(cls)) arg->blame(MIM_X86_BE "too many arguments are not supported yet").bail();
            moves.emplace_back(Reg{arg_reg(cls, c++), cls}, source(x));
        }
    }
    parallel(std::move(moves), {});
    // A variadic callee reads the number of vector registers used from `al`.
    line("movl ${}, %eax", f);
    line("call {}", symbol(callee));

    auto ret = arg->proj(n, n - 1);
    if (ret == ret_) return epilogue();

    auto k    = Lam::isa_mut_basicblock(ret);
    auto m    = k->num_vars();
    auto back = Moves();
    for (size_t i = 0; i != m; ++i)
        if (auto x = k->var(m, i); !is_mem(x)) {
            auto cls = Liveness::cls(x);
            if (auto r = fe::lookup(regs_, x)) back.emplace_back(Reg{*r, cls}, Src{.reg = {ret_reg(cls), cls}});
        }
    parallel(std::move(back), {});
    jump(k);
}

/// The block that @p b should fall through to.
Lam* Emitter::tail(const Liveness::Block& b) const {
    auto term   = b.lam->body()->as<App>();
    auto callee = term->callee();
    if (callee == ret_) return nullptr;
    if (Lam::isa_mut_returning(callee)) {
        auto ret = term->arg(term->num_args(), term->num_args() - 1);
        return ret == ret_ ? nullptr : Lam::isa_mut_basicblock(ret);
    }
    if (auto ex = callee->isa<Extract>()) {
        if (ex->tuple()->num_projs() != 2) return nullptr;
        auto [e, t] = ex->tuple()->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) return nullptr;
        return takes_values(t) ? t : e;
    }
    return Lam::isa_mut_basicblock(callee);
}

void Emitter::block(const Liveness::Block& b) {
    for (auto insn : b.insns)
        step(insn);

    auto term   = b.lam->body()->as<App>();
    auto arg    = term->arg();
    auto callee = term->callee();
    if (callee == ret_) {
        auto moves = Moves();
        for (auto a : term->args()) {
            if (is_mem(a)) continue;
            auto cls = Liveness::cls(strip(a));
            moves.emplace_back(Reg{ret_reg(cls), cls}, source(a));
        }
        if (moves.size() > 1) b.lam->blame(MIM_X86_BE "`{}` returns more than one value", b.lam).bail();
        parallel(std::move(moves), {});
        epilogue();
    } else if (auto f = Lam::isa_mut_returning(callee)) {
        call(f, arg);
    } else if (auto ex = callee->isa<Extract>()) {
        auto targets = ex->tuple();
        if (targets->num_projs() != 2) b.lam->blame(MIM_X86_BE "only two-way branches are supported yet").bail();
        auto [e, t] = targets->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) b.lam->blame(MIM_X86_BE "`{}` branches to something other than blocks", b.lam).bail();
        auto c          = ex->index();
        auto trampoline = takes_values(t);
        auto t_lbl      = trampoline ? label(b.lam) + ".t" : label(t);
        if (auto app = isa_sched_cc(c)) {
            use_flags(app->arg());
            line("j{} {}", ccs[Axm::as<cc, 1>(sched_insn(app)).sub()], t_lbl);
        } else {
            auto r = name(use_reg(c), 8);
            line("testb {}, {}", r, r);
            line("jne {}", t_lbl);
        }
        copy(e, arg);
        if (trampoline) {
            line("jmp {}", label(e));
            std::println(body_, "{}:", t_lbl);
            copy(t, arg);
            jump(t);
        } else {
            jump(e);
        }
    } else if (auto bb = Lam::isa_mut_basicblock(callee)) {
        copy(bb, arg);
        jump(bb);
    } else {
        b.lam->blame(MIM_X86_BE "cannot emit the jump to `{}`", callee).bail();
    }
}

void Emitter::emit(bool global) {
    if (!ret_) fun_->blame(MIM_X86_BE "`{}` does not end in a return continuation", fun_).bail();

    auto done = DefSet();
    for (const auto& b : live_.blocks()) {
        for (auto insn : b.insns) {
            collect(insn->callee(), done);
            collect(insn->arg(), done);
            for (auto [j, r] : constraint(insn).results)
                regs_.emplace(result(insn, j), *Lit::isa(r));
        }
        collect(b.lam->body(), done);
    }
    auto used = std::array<bool, Num_Regs>{};
    for (auto [def, r] : regs_)
        if (Liveness::cls(def) == Liveness::Gpr) used[r] = true;
    for (size_t r = 0; r != Num_Regs; ++r)
        if (!Caller_Saved[r] && !reserved(r) && used[r]) saved_.emplace_back(r);

    auto order  = fe::Vector<const Liveness::Block*>();
    auto placed = LamSet();
    for (auto queue = std::deque<Lam*>{fun_}; !queue.empty();) {
        for (auto lam = queue.front(); lam && placed.emplace(lam).second;) {
            auto b = live_.block(lam);
            order.emplace_back(b);
            for (auto s : b->succs)
                queue.emplace_back(live_.blocks()[s].lam);
            lam = tail(*b);
            if (lam && !live_.block(lam)) lam = nullptr;
        }
        queue.pop_front();
    }

    auto n     = fun_->num_vars();
    auto moves = Moves();
    for (size_t i = 0, a = 0, f = 0; i + 1 < n; ++i) {
        if (auto x = fun_->var(n, i); !is_mem(x)) {
            auto cls = Liveness::cls(x);
            auto& c  = cls == Liveness::Gpr ? a : f;
            if (c == num_arg_regs(cls)) fun_->blame(MIM_X86_BE "too many arguments are not supported yet").bail();
            if (auto r = fe::lookup(regs_, x)) moves.emplace_back(Reg{*r, cls}, Src{.reg = {arg_reg(cls, c), cls}});
            ++c;
        }
    }
    parallel(std::move(moves), {});

    for (size_t i = 0; i != order.size(); ++i) {
        auto b = order[i];
        next_  = i + 1 != order.size() ? order[i + 1]->lam : nullptr;
        if (i != 0) std::println(body_, "{}:", label(b->lam));
        flags_ = nullptr;
        block(*b);
    }

    auto sym    = symbol(fun_);
    auto pushed = 8 * saved_.size();
    auto frame  = fe::pad(8 * num_slots_ + pushed, 16) - pushed;
    if (global) std::println(os_, "\t.globl {}", sym);
    std::println(os_, "\t.type {}, @function", sym);
    std::println(os_, "{}:", sym);
    std::println(os_, "\tpushq %rbp");
    std::println(os_, "\tmovq %rsp, %rbp");
    for (auto r : saved_)
        std::println(os_, "\tpushq {}", name(r, 64));
    if (frame != 0) std::println(os_, "\tsubq ${}, %rsp", frame);
    os_ << body_.view();
    std::println(os_, "\t.size {}, .-{}", sym, sym);
}

} // namespace

void Emit::start() {
    auto path = (world().name() ? world().name().str() : "a"s) + ".s";
    if (auto o = arg_value(args(), "o", "output")) path = *o;
    auto out = Out(path);
    auto& os = *out.os();

    auto muts = world().externals().mutate();
    std::ranges::sort(muts, {}, [](Def* mut) { return mut->sym().str(); });

    std::println(os, "\t.text");
    auto funs = Funs();
    for (auto mut : muts) {
        auto lam = mut->isa<Lam>();
        if (!lam) mut->blame(MIM_X86_BE "cannot emit `{}`", mut).bail();
        if (lam->is_set()) {
            funs.seen.emplace(lam);
            Emitter(os, funs, lam).emit(true);
        }
    }
    while (!funs.todo.empty()) {
        auto lam = funs.todo.front();
        funs.todo.pop_front();
        Emitter(os, funs, lam).emit(false);
    }
    if (!funs.pool.empty()) {
        auto constants = fe::Vector<std::pair<u64, u64>>(funs.pool.size());
        for (auto& [c, i] : funs.pool)
            constants[i] = c;
        std::println(os, "\t.section .rodata");
        std::println(os, "\t.p2align 4");
        for (size_t i = 0; i != constants.size(); ++i) {
            std::println(os, ".Lpool{}:", i);
            std::println(os, "\t.quad 0x{:016x}", constants[i].first);
            std::println(os, "\t.quad 0x{:016x}", constants[i].second);
        }
    }
    std::println(os, "\t.section .note.GNU-stack,\"\",@progbits");
}

} // namespace mim::plug::x86
