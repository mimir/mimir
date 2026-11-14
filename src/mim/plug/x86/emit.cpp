#include "mim/plug/x86/emit.h"

#include <bit>
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

constexpr std::array<Reg, 6> arg_regs = {
    Reg{"%dil",  "%di", "%edi", "%rdi"},
    Reg{"%sil",  "%si", "%esi", "%rsi"},
    Reg{ "%dl",  "%dx", "%edx", "%rdx"},
    rcx,
    Reg{"%r8b", "%r8w", "%r8d",  "%r8"},
    Reg{"%r9b", "%r9w", "%r9d",  "%r9"},
};

constexpr std::array<std::string_view, 16> ccs
    = {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"};

size_t log_bytes(nat_t w) { return std::countr_zero(w / 8); }
std::string_view name(const Reg& reg, nat_t w) { return reg[log_bytes(w)]; }
char suffix(nat_t w) { return "bwlq"[log_bytes(w)]; }

bool is_mem(const Def* def) { return Axm::isa<core::M>(def->type()); }

/// Width of the register holding a value of @p type.
nat_t width(const Def* type) { return std::bit_ceil(std::max<nat_t>(Idx::expect_bitwidth(type, "an integer"), 8)); }

nat_t nat(const Def* def) {
    if (auto l = Lit::isa(def)) return *l;
    def->blame(MIM_X86_BE "expected a literal, got `{}`", def).bail();
}

/// The `f` of `x86.sched f m x`.
const Def* sched_insn(const App* app) { return app->callee()->as<App>()->callee()->as<App>()->arg(); }

class Emitter {
public:
    explicit Emitter(std::ostream& os)
        : os_(os) {}

    void emit(Lam*);

private:
    template<class... Args>
    void line(std::format_string<Args...> fmt, Args&&... args) {
        std::print(body_, "\t");
        std::println(body_, fmt, std::forward<Args>(args)...);
    }

    static std::string slot(size_t i) { return std::format("-{}(%rbp)", 8 * (i + 1)); }
    const Def* prev_mem(const App*);
    void get(const Reg&, const Def*);
    void put(const Def*, const Reg&);
    void flags(const Def*);
    void step(const App*);
    void insn(const Def* res, const Def* f, const Def* x);

    std::ostream& os_;
    std::ostringstream body_;
    DefMap<size_t> slots_;
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

void Emitter::put(const Def* def, const Reg& reg) {
    auto w      = width(def->type());
    auto i      = slots_.size();
    slots_[def] = i;
    line("mov{} {}, {}", suffix(w), name(reg, w), slot(i));
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

void Emitter::insn(const Def* res, const Def* f, const Def* x) {
    auto axm = f->as<App>()->axm();
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
    auto extend = [&](char kind) {
        auto ws = width(x->type());
        auto wd = width(res->type());
        get(rax, x);
        if (ws < wd && !(kind == 'z' && ws == 32))
            line("mov{}{}{} {}, {}", kind, suffix(ws), suffix(wd), name(rax, ws), name(rax, wd));
        put(res, rax);
    };

    // clang-format off
    if      (id == Annex::Base<add>)   binary("add");
    else if (id == Annex::Base<sub>)   binary("sub");
    else if (id == Annex::Base<and_>)  binary("and");
    else if (id == Annex::Base<or_>)   binary("or");
    else if (id == Annex::Base<xor_>)  binary("xor");
    else if (id == Annex::Base<imul>)  binary("imul");
    else if (id == Annex::Base<shl>)   shift("shl");
    else if (id == Annex::Base<shr>)   shift("shr");
    else if (id == Annex::Base<sar>)   shift("sar");
    else if (id == Annex::Base<movzx>) extend('z');
    else if (id == Annex::Base<movsx>) extend('s');
    // clang-format on
    else if (id == Annex::Base<trunc>) {
        auto w = width(res->type());
        get(rax, x);
        auto n = *Idx::isa_lit(res->type());
        if (w == 64 ? n != 0 : n != 1_n << w) {
            if (!std::has_single_bit(n))
                res->blame(MIM_X86_BE "truncation to `{}` is not supported", res->type()).bail();
            line("and{} ${}, {}", suffix(w), n - 1, name(rax, w));
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
    if (Axm::isa<load>(app) || Axm::isa<store>(app)) return app->arg()->proj(app->num_args(), 0);
    app->blame(MIM_X86_BE "unexpected memory producer `{}`", app).bail();
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), sched_insn(app), app->arg());

    if (Axm::isa<load>(app)) {
        auto [_, p] = app->args<2>();
        auto res    = app->proj(2, 1);
        auto w      = width(res->type());
        get(rax, p);
        line("mov{} (%rax), {}", suffix(w), name(rcx, w));
        return put(res, rcx);
    }

    auto [_, p, v] = app->args<3>();
    auto w         = width(v->type());
    get(rax, p);
    get(rcx, v);
    line("mov{} {}, (%rax)", suffix(w), name(rcx, w));
}

void Emitter::emit(Lam* lam) {
    auto n   = lam->num_vars();
    auto ret = n == 0 ? nullptr : lam->var(n, n - 1);
    if (!ret || !Pi::isa_cn(ret->type()))
        lam->blame(MIM_X86_BE "`{}` does not end in a return continuation", lam).bail();

    const Def* mem = nullptr;
    size_t a       = 0;
    for (size_t i = 0; i + 1 < n; ++i) {
        auto var = lam->var(n, i);
        if (is_mem(var)) {
            mem = var;
        } else {
            if (a == arg_regs.size()) lam->blame(MIM_X86_BE "more than six arguments are not supported yet").bail();
            put(var, arg_regs[a++]);
        }
    }

    auto term = lam->body()->isa<App>();
    if (!term || term->callee() != ret) lam->blame(MIM_X86_BE "control flow in `{}` is not supported yet", lam).bail();

    auto arg       = term->arg();
    auto k         = arg->num_projs();
    const Def* end = nullptr;
    auto vals      = DefVec();
    for (size_t i = 0; i != k; ++i)
        if (auto d = arg->proj(k, i); is_mem(d))
            end = d;
        else
            vals.emplace_back(d);
    if (!mem || !end) lam->blame(MIM_X86_BE "`{}` has no `mem.M` chain", lam).bail();
    if (vals.size() > 1) lam->blame(MIM_X86_BE "`{}` returns more than one value", lam).bail();

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

    if (!vals.empty()) get(rax, vals.front());
    line("leave");
    line("ret");

    auto sym   = lam->sym().str();
    auto frame = (8 * slots_.size() + 15) / 16 * 16;
    std::println(os_, "\t.globl {}", sym);
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
    for (auto mut : muts)
        if (auto lam = mut->isa<Lam>(); lam && lam->is_set())
            Emitter(os).emit(lam);
        else
            mut->blame(MIM_X86_BE "cannot emit `{}`", mut).bail();
    std::println(os, "\t.section .note.GNU-stack,\"\",@progbits");
}

} // namespace mim::plug::x86
