#include "mim/plug/x86/emit.h"

#include <bit>
#include <deque>
#include <print>
#include <ranges>
#include <sstream>

#include <mim/plugin.h>

#include <mim/plug/core/core.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

using namespace std::string_literals;

namespace core = mim::plug::core;

namespace {

using Reg = std::array<std::string_view, 4>;

constexpr Reg rax = {"%al", "%ax", "%eax", "%rax"};
constexpr Reg rcx = {"%cl", "%cx", "%ecx", "%rcx"};
constexpr Reg rdx = {"%dl", "%dx", "%edx", "%rdx"};

constexpr std::array<Reg, 6> arg_regs = {
    Reg{"%dil",  "%di", "%edi", "%rdi"},
    Reg{"%sil",  "%si", "%esi", "%rsi"},
    rdx, rcx,
    Reg{"%r8b", "%r8w", "%r8d",  "%r8"},
    Reg{"%r9b", "%r9w", "%r9d",  "%r9"},
};

constexpr std::array<std::string_view, 16> ccs
    = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};

size_t log_bytes(nat_t w) { return std::countr_zero(w / 8); }
std::string_view name(const Reg& reg, nat_t w) { return reg[log_bytes(w)]; }
char suffix(nat_t w) { return "bwlq"[log_bytes(w)]; }

bool is_mem(const Def* def) { return Axm::isa<core::M>(def->type()); }

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

/// Width of the register holding a value of @p type.
nat_t width(const Def* type) { return std::bit_ceil(std::max<nat_t>(Idx::expect_bitwidth(type, "an integer"), 8)); }

nat_t nat(const Def* def) {
    if (auto l = Lit::isa(def)) return *l;
    def->blame(MIM_X86_BE "expected a literal, got `{}`", def).bail();
}

/// The `f` of `x86.sched f m x`.
const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }

/// Functions still to emit, shared by all Emitter%s.
struct Funs {
    std::deque<Lam*> todo;
    LamSet seen;

    void push(Lam* lam) {
        if (seen.emplace(lam).second) todo.emplace_back(lam);
    }
};

class Emitter {
public:
    Emitter(std::ostream& os, Funs& funs)
        : os_(os)
        , funs_(funs) {}

    void emit(Lam*, bool global);

private:
    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::print(body_, "\t");
        std::println(body_, fmt, std::forward<Args>(args)...);
    }

    static std::string slot(size_t i) { return std::format("-{}(%rbp)", 8 * (i + 1)); }
    static std::string label(Lam* lam) { return std::format(".L{}", lam->unique_name()); }
    std::string symbol(Lam*);
    size_t slot_of(const Def*);
    const Def* prev_mem(const App*);
    void get(const Reg&, const Def*);
    void put(const Def*, const Reg&);
    void flags(const Def*);
    std::string operand(const Def* addr);
    void step(const App*);
    void insn(const Def* res, const Def* f, const Def* x);
    void block(Lam*);
    void schedule(Lam*, const Def* end);
    Lam* target(const Def*);
    void jump(Lam*, const Def* arg);
    void call(Lam* bb, Lam* callee, const Def* arg);

    std::ostream& os_;
    Funs& funs_;
    std::ostringstream body_;
    DefMap<size_t> slots_;
    size_t num_slots_ = 0;
    const Def* ret_   = nullptr;
    std::deque<Lam*> blocks_;
    LamSet seen_;
};

void Emitter::get(const Reg& reg, const Def* def) {
    auto w = width(def->type());
    if (auto l = Lit::isa(def)) {
        auto s = std::bit_cast<int64_t>(*l);
        if (w == 64 && (s < INT32_MIN || s > INT32_MAX))
            line("movabsq ${}, {}", s, name(reg, w));
        else if (w == 64)
            line("movq ${}, {}", s, name(reg, w));
        else
            line("mov{} ${}, {}", suffix(w), *l, name(reg, w));
    } else if (auto i = slots_.find(def); i != slots_.end()) {
        line("mov{} {}, {}", suffix(w), slot(i->second), name(reg, w));
    } else {
        def->blame(MIM_X86_BE "value `{}` is not available here", def).bail();
    }
}

size_t Emitter::slot_of(const Def* def) {
    auto [i, ins] = slots_.emplace(def, num_slots_);
    if (ins) ++num_slots_;
    return i->second;
}

void Emitter::put(const Def* def, const Reg& reg) {
    auto w = width(def->type());
    line("mov{} {}, {}", suffix(w), name(reg, w), slot(slot_of(def)));
}

/// Recomputes the flags of a `cmp` or `test` right before their consumer, so nothing in between can clobber them.
void Emitter::flags(const Def* fl) {
    if (auto ex = fl->isa<Extract>())
        if (auto app = ex->tuple()->isa<App>(); app && Axm::isa<sched>(app)) {
            auto f = sched_insn(app);
            if (auto axm = f->as<App>()->axm()) {
                auto mnemonic = axm->flags() == Annex::Base<cmp>  ? "cmp"
                              : axm->flags() == Annex::Base<test> ? "test"
                                                                  : "";
                if (*mnemonic) {
                    auto [a, b] = app->arg()->projs<2>();
                    auto w      = width(a->type());
                    get(rax, a);
                    get(rcx, b);
                    line("{}{} {}, {}", mnemonic, suffix(w), name(rcx, w), name(rax, w));
                    return;
                }
            }
        }
    fl->blame(MIM_X86_BE "flags `{}` do not come from a scheduled `cmp` or `test`", fl).bail();
}

/// Loads base and index of @p addr into `rax` and `rdx`.
std::string Emitter::operand(const Def* addr) {
    auto [disp, base, index, scale] = Axm::as<x86::addr>(addr)->args<4>();
    auto d                          = std::bit_cast<int32_t>(u32(nat(disp)));
    auto has_base                   = Lit::isa(base) != 0;
    auto has_index                  = Lit::isa(index) != 0;
    if (!has_base && !has_index) return std::to_string(d);

    auto res = d != 0 ? std::to_string(d) : ""s;
    if (has_base) get(rax, base);
    res += has_base ? "(%rax" : "(";
    if (has_index) {
        get(rdx, index);
        res += std::format(",%rdx,{}", 1 << nat(scale));
    }
    return res + ")";
}

void Emitter::insn(const Def* res, const Def* f, const Def* x) {
    auto axm = f->isa<Axm>() ? f->as<Axm>() : f->as<App>()->axm();
    auto id  = axm->flags();

    auto binary = [&](std::string_view mnemonic) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        get(rax, a);
        get(rcx, b);
        line("{}{} {}, {}", mnemonic, suffix(w), name(rcx, w), name(rax, w));
        put(res, rax);
    };
    auto shift = [&](std::string_view mnemonic) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        get(rax, a);
        get(rcx, b);
        line("{}{} %cl, {}", mnemonic, suffix(w), name(rax, w));
        put(res, rax);
    };
    auto divide = [&](bool sign, const Reg& out) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        get(rax, a);
        get(rcx, b);
        if (!sign)
            line("xorl %edx, %edx");
        else
            line("{}", w == 64 ? "cqto" : w == 32 ? "cltd" : "cwtd");
        line("{}div{} {}", sign ? "i" : "", suffix(w), name(rcx, w));
        put(res, out);
    };
    auto extend = [&](char kind) {
        auto ws = width(x->type());
        auto wd = width(res->type());
        get(rax, x);
        if (ws < wd && !(kind == 'z' && ws == 32))
            line("mov{}{}{} {}, {}", kind, suffix(ws), suffix(wd), name(rax, ws), name(rax, wd));
        put(res, rax);
    };

    // clang-format off
    if      (auto mn = alu(id); !mn.empty()) binary(mn);
    else if (id == Annex::Base<div>)   divide(false, rax);
    else if (id == Annex::Base<rem>)   divide(false, rdx);
    else if (id == Annex::Base<idiv>)  divide(true, rax);
    else if (id == Annex::Base<irem>)  divide(true, rdx);
    else if (id == Annex::Base<shl>)   shift("shl");
    else if (id == Annex::Base<shr>)   shift("shr");
    else if (id == Annex::Base<sar>)   shift("sar");
    else if (id == Annex::Base<movzx>) extend('z');
    else if (id == Annex::Base<movsx>) extend('s');
    // clang-format on
    else if (id == Annex::Base<lea>) {
        line("leaq {}, %rax", operand(x));
        put(res, rax);
    } else if (id == Annex::Base<trunc>) {
        auto w = width(res->type());
        get(rax, x);
        auto n    = *Idx::isa_lit(res->type());
        auto full = w == 64 ? 0 : 1_n << w;
        if (std::has_single_bit(n) && n != full) {
            line("and{} ${}, {}", suffix(w), n - 1, name(rax, w));
        } else if (n != 0 && !std::has_single_bit(n)) {
            auto ws = std::max<nat_t>(width(x->type()), 32);
            if (auto wx = width(x->type()); wx < 32) line("movz{}l {}, %eax", suffix(wx), name(rax, wx));
            line("xorl %edx, %edx");
            line("mov{} ${}, {}", suffix(ws), n, name(rcx, ws));
            line("div{} {}", suffix(ws), name(rcx, ws));
            line("movl %edx, %eax");
        }
        put(res, rax);
    } else if (id == Annex::Base<cmp> || id == Annex::Base<test>) {
        // Rematerialized by each consumer.
    } else if (id == Annex::Base<cc>) {
        flags(x);
        line("set{} %al", ccs[nat(f->as<App>()->arg())]);
        put(res, rax);
    } else if (id == Annex::Base<cmovcc>) {
        auto [fl, a, b] = x->projs<3>();
        auto w          = width(res->type());
        flags(fl);
        get(rax, a);
        get(rcx, b);
        line("cmov{} {}, {}", ccs[nat(f->as<App>()->callee()->as<App>()->arg())], name(rcx, w), name(rax, w));
        put(res, rax);
    } else {
        f->blame(MIM_X86_BE "cannot emit `{}`", axm).bail();
    }
}

const Def* Emitter::prev_mem(const App* app) {
    if (Axm::isa<sched>(app)) return app->callee()->as<App>()->arg();
    if (Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<x86::slot>(app) || Axm::isa<rm>(app)
        || Axm::isa<mr>(app))
        return app->arg()->proj(app->num_args(), 0);
    app->blame(MIM_X86_BE "unexpected memory producer `{}`", app).bail();
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), sched_insn(app), app->arg());

    if (Axm::isa<x86::slot>(app)) {
        num_slots_ += (nat(app->callee()->as<App>()->arg()) + 7) / 8;
        line("leaq {}, %rax", slot(num_slots_ - 1));
        return put(app->proj(2, 1), rax);
    }

    if (auto op = Axm::isa<rm>(app)) {
        auto [_, a, addr] = app->args<3>();
        auto res          = app->proj(2, 1);
        auto w            = width(a->type());
        auto mem          = operand(addr);
        get(rcx, a);
        line("{}{} {}, {}", alu(op->decurry()->arg()->as<App>()->axm()->flags()), suffix(w), mem, name(rcx, w));
        return put(res, rcx);
    }

    if (auto op = Axm::isa<mr>(app)) {
        auto [_, addr, b] = app->args<3>();
        auto w            = width(b->type());
        auto mem          = operand(addr);
        get(rcx, b);
        line("{}{} {}, {}", alu(op->decurry()->arg()->as<App>()->axm()->flags()), suffix(w), name(rcx, w), mem);
        return;
    }

    if (Axm::isa<load>(app)) {
        auto [_, p] = app->args<2>();
        auto res    = app->proj(2, 1);
        auto w      = width(res->type());
        line("mov{} {}, {}", suffix(w), operand(p), name(rcx, w));
        return put(res, rcx);
    }

    auto [_, p, v] = app->args<3>();
    auto w         = width(v->type());
    auto a         = operand(p);
    get(rcx, v);
    line("mov{} {}, {}", suffix(w), name(rcx, w), a);
}

std::string Emitter::symbol(Lam* lam) {
    if (lam->is_external() || !lam->is_set()) return lam->sym().str();
    funs_.push(lam);
    return lam->unique_name();
}

void Emitter::schedule(Lam* bb, const Def* end) {
    auto n         = bb->num_vars();
    const Def* mem = nullptr;
    for (size_t i = 0; i != n && !mem; ++i)
        if (auto var = bb->var(n, i); is_mem(var)) mem = var;
    if (!mem || !end) bb->blame(MIM_X86_BE "`{}` has no `mem.M` chain", bb).bail();

    auto chain = std::vector<const App*>();
    for (auto m = end; m != mem;) {
        auto node = m->isa<Extract>() ? m->as<Extract>()->tuple() : m;
        auto app  = node->isa<App>();
        if (!app) m->blame(MIM_X86_BE "unexpected memory `{}`", m).bail();
        chain.emplace_back(app);
        m = prev_mem(app);
    }
    for (auto app : chain | std::views::reverse)
        step(app);
}

Lam* Emitter::target(const Def* def) {
    auto bb = Lam::isa_mut_basicblock(def);
    if (!bb || !bb->is_set()) def->blame(MIM_X86_BE "`{}` is not a basic block", def).bail();
    if (seen_.emplace(bb).second) blocks_.emplace_back(bb);
    return bb;
}

void Emitter::jump(Lam* bb, const Def* arg) {
    auto n     = bb->num_vars();
    auto moves = std::vector<std::pair<const Def*, const Def*>>();
    for (size_t i = 0; i != n; ++i)
        if (auto var = bb->var(n, i); !is_mem(var)) moves.emplace_back(var, arg->proj(n, i));

    // A loop may pass its own parameters around, so read all of them before overwriting any.
    auto own = [bb](auto move) {
        return move.second->template isa<Var>() && move.second->template as<Var>()->binder() == bb;
    };
    auto tmps = std::vector<size_t>();
    if (std::ranges::any_of(moves, own)) {
        for (auto [var, val] : moves) {
            auto w = width(val->type());
            get(rax, val);
            tmps.emplace_back(num_slots_++);
            line("mov{} {}, {}", suffix(w), name(rax, w), slot(tmps.back()));
        }
    }
    for (size_t i = 0; i != moves.size(); ++i) {
        auto [var, val] = moves[i];
        if (tmps.empty()) {
            get(rax, val);
        } else {
            auto w = width(val->type());
            line("mov{} {}, {}", suffix(w), slot(tmps[i]), name(rax, w));
        }
        put(var, rax);
    }
    line("jmp {}", label(bb));
}

void Emitter::call(Lam* bb, Lam* callee, const Def* arg) {
    auto pi = Pi::isa_returning(callee->type());
    auto n  = pi->num_doms();
    for (size_t i = 0, a = 0; i + 1 < n; ++i) {
        if (auto x = arg->proj(n, i); !is_mem(x)) {
            if (a == arg_regs.size()) bb->blame(MIM_X86_BE "more than six arguments are not supported yet").bail();
            get(arg_regs[a++], x);
        }
    }
    // A variadic callee reads the number of vector registers used from `al`.
    line("movl $0, %eax");
    line("call {}", symbol(callee));

    auto ret = arg->proj(n, n - 1);
    if (ret == ret_) {
        line("leave");
        line("ret");
        return;
    }

    auto k = target(ret);
    auto m = k->num_vars();
    for (size_t i = 0; i != m; ++i)
        if (auto var = k->var(m, i); !is_mem(var)) put(var, rax);
    line("jmp {}", label(k));
}

void Emitter::block(Lam* bb) {
    auto term = bb->body()->isa<App>();
    if (!term) bb->blame(MIM_X86_BE "`{}` does not end in a jump", bb).bail();

    auto arg       = term->arg();
    auto callee    = term->callee();
    auto k         = arg->num_projs();
    const Def* end = nullptr;
    for (size_t i = 0; i != k; ++i)
        if (auto d = arg->proj(k, i); is_mem(d)) end = d;
    schedule(bb, end);

    if (callee == ret_) {
        auto vals = DefVec();
        for (size_t i = 0; i != k; ++i)
            if (auto d = arg->proj(k, i); !is_mem(d)) vals.emplace_back(d);
        if (vals.size() > 1) bb->blame(MIM_X86_BE "`{}` returns more than one value", bb).bail();
        if (!vals.empty()) get(rax, vals.front());
        line("leave");
        line("ret");
    } else if (auto f = Lam::isa_mut_returning(callee)) {
        call(bb, f, arg);
    } else if (auto ex = callee->isa<Extract>()) {
        auto targets = ex->tuple();
        if (targets->num_projs() != 2) bb->blame(MIM_X86_BE "only two-way branches are supported yet").bail();
        auto [e, t] = targets->projs<2>([this](const Def* d) { return target(d); });
        auto c      = ex->index();
        auto app    = c->isa<Extract>() ? c->as<Extract>()->tuple()->isa<App>() : nullptr;
        auto copy   = std::ranges::any_of(t->vars(), [](const Def* var) { return !is_mem(var); });
        auto t_lbl  = copy ? label(bb) + ".t" : label(t);
        if (app && Axm::isa<sched>(app) && sched_insn(app)->as<App>()->axm()->flags() == Annex::Base<x86::cc>) {
            flags(app->arg());
            line("j{} {}", ccs[nat(sched_insn(app)->as<App>()->arg())], t_lbl);
        } else {
            get(rax, c);
            line("testb %al, %al");
            line("jne {}", t_lbl);
        }
        jump(e, arg);
        if (copy) {
            std::println(body_, "{}:", t_lbl);
            jump(t, arg);
        }
    } else {
        jump(target(callee), arg);
    }
}

void Emitter::emit(Lam* fun, bool global) {
    auto n = fun->num_vars();
    ret_   = fun->ret_var();
    if (!ret_) fun->blame(MIM_X86_BE "`{}` does not end in a return continuation", fun).bail();

    for (size_t i = 0, a = 0; i + 1 < n; ++i) {
        if (auto var = fun->var(n, i); !is_mem(var)) {
            if (a == arg_regs.size()) fun->blame(MIM_X86_BE "more than six arguments are not supported yet").bail();
            put(var, arg_regs[a++]);
        }
    }

    block(fun);
    while (!blocks_.empty()) {
        auto bb = blocks_.front();
        blocks_.pop_front();
        std::println(body_, "{}:", label(bb));
        block(bb);
    }

    auto sym   = symbol(fun);
    auto frame = (8 * num_slots_ + 15) / 16 * 16;
    if (global) std::println(os_, "\t.globl {}", sym);
    std::println(os_, "\t.type {}, @function", sym);
    std::println(os_, "{}:", sym);
    std::println(os_, "\tpushq %rbp");
    std::println(os_, "\tmovq %rsp, %rbp");
    if (frame != 0) std::println(os_, "\tsubq ${}, %rsp", frame);
    std::print(os_, "{}", body_.str());
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
            Emitter(os, funs).emit(lam, true);
        }
    }
    while (!funs.todo.empty()) {
        auto lam = funs.todo.front();
        funs.todo.pop_front();
        Emitter(os, funs).emit(lam, false);
    }
    std::println(os, "\t.section .note.GNU-stack,\"\",@progbits");
}

} // namespace mim::plug::x86
