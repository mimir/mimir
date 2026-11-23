#include "mim/plug/arm/emit.h"

#include <print>

#include <mim/plugin.h>

#include <mim/native/emit.h>

#include "mim/plug/arm/target.h"

namespace mim::plug::arm {

using native::Reg;

namespace {

/// The AAPCS64 condition codes in hardware order; `arm.cc`'s subtag indexes this table.
constexpr std::array<std::string_view, 16> ccs
    = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};

/// `x17` holds a literal operand; `x16` holds a second one, breaks a copy cycle, or carries a spill slot.
constexpr size_t Scratch = encoding(reg::x16), Scratch2 = encoding(reg::x17);

/// The bit width of the register holding a value of @p type.
nat_t width(const Def* type) { return Idx::expect_bitwidth(type, "an integer"); }

/// The `b`/`h` suffix of a load or store of width @p w.
std::string_view suffix(nat_t w) { return w <= 8 ? "b" : w <= 16 ? "h" : ""; }

std::string reg(size_t r, nat_t w) { return std::format("{}{}", w == 64 ? 'x' : 'w', r); }

/// Writes one function in AArch64 syntax.
class Emitter : public native::Emitter {
public:
    Emitter(std::ostream& os, Emit& emit, Lam* fun)
        : native::Emitter(arm::target(), os, emit, fun) {}

private:
    /// The frame address @p off bytes above `sp`, also while push_slot() moved `sp` down.
    std::string at(size_t off) const { return std::format("[sp, #{}]", off + pushed_); }
    std::string slot(size_t i) const { return at(8 * (saved().size() + i)); }
    /// The register operand @p def of width @p w; a literal goes into @p scratch.
    std::string operand(const Def* def, nat_t w, size_t scratch);
    /// The second operand @p b after @p a: an immediate below @p limit, otherwise a register.
    std::string operand(const Def* a, const Def* b, nat_t w, nat_t limit);
    void insn(const Def* res, const Def* f, const Def* x);

    void step(const App*) final;
    void move(Reg dst, Reg src) final {
        if (dst != src) line("mov x{}, x{}", dst.num, src.num);
    }
    std::optional<Reg> scratch(Target::Class) const final { return Reg{Scratch}; }
    void mov_imm(Reg dst, const Def* lit) final {
        line("ldr {}, ={}", reg(dst.num, width(lit->type())), *Lit::isa(lit));
    }
    void push_slot(size_t i) final {
        line("ldr x{}, {}", Scratch, slot(i));
        line("str x{}, [sp, #-16]!", Scratch);
        pushed_ += 16;
    }
    void pop_slot(size_t i) final {
        line("ldr x{}, [sp], #16", Scratch);
        pushed_ -= 16;
        line("str x{}, {}", Scratch, slot(i));
    }
    void jmp(std::string_view label) final { line("b {}", label); }
    void branch_cc(size_t cc, std::string_view label) final { line("b.{} {}", ccs[cc], label); }
    void branch_if(size_t r, std::string_view label) final { line("cbnz w{}, {}", r, label); }
    void call_insn(std::string_view sym, const std::array<size_t, Target::Num_Classes>&) final { line("bl {}", sym); }
    void spill(const Def* x, size_t i) final { line("str {}, {}", operand(x, 64, Scratch2), slot(i)); }
    void reload(const Def* res, size_t i) final { line("ldr x{}, {}", def_reg(res), slot(i)); }
    void slot_addr(size_t dst, size_t first, size_t) final;
    void prologue(std::ostream& os) final;
    void epilogue() final;
    std::string_view function_type() const final { return "%function"; }

    size_t pushed_ = 0;
};

std::string Emitter::operand(const Def* def, nat_t w, size_t scratch) {
    if (!Lit::isa(def)) return reg(use_reg(def), w);
    mov_imm(Reg{scratch}, def);
    return reg(scratch, w);
}

std::string Emitter::operand(const Def* a, const Def* b, nat_t w, nat_t limit) {
    if (auto l = Lit::isa(b); l && *l < limit) return std::format("#{}", *l);
    return operand(b, w, Lit::isa(a) ? Scratch : Scratch2);
}

void Emitter::insn(const Def* res, const Def* f, const Def* x) {
    auto [axm, curry, _] = Axm::get(f);
    auto id              = axm->base();

    auto three = [&](std::string_view mnemonic, nat_t limit) {
        auto [a, b] = x->projs<2>();
        auto w      = width(res->type());
        auto lhs    = operand(a, w, Scratch2);
        auto rhs    = operand(a, b, w, limit);
        line("{} {}, {}, {}", mnemonic, reg(def_reg(res), w), lhs, rhs);
    };
    constexpr nat_t Imm12 = 4096;

    if (id == Annex::Base<add>)
        three("add", Imm12);
    else if (id == Annex::Base<sub>)
        three("sub", Imm12);
    else if (id == Annex::Base<and_>)
        three("and", 0);
    else if (id == Annex::Base<or_>)
        three("orr", 0);
    else if (id == Annex::Base<xor_>)
        three("eor", 0);
    else if (id == Annex::Base<mul>)
        three("mul", 0);
    else if (id == Annex::Base<madd>) {
        auto w       = width(res->type());
        auto scratch = std::array{Scratch2, Scratch};
        auto next    = size_t(0);
        auto ops     = x->projs<3>([&](const Def* op) { return operand(op, w, scratch[Lit::isa(op) ? next++ : 0]); });
        line("madd {}, {}, {}, {}", reg(def_reg(res), w), ops[0], ops[1], ops[2]);
    } else if (id == Annex::Base<udiv>)
        three("udiv", 0);
    else if (id == Annex::Base<sdiv>)
        three("sdiv", 0);
    else if (id == Annex::Base<shl>)
        three("lsl", width(res->type()));
    else if (id == Annex::Base<shr>)
        three("lsr", width(res->type()));
    else if (id == Annex::Base<sar>)
        three("asr", width(res->type()));
    else if (id == Annex::Base<zext> || id == Annex::Base<sext>) {
        auto ws   = width(x->type());
        auto wd   = width(res->type());
        auto rs   = use_reg(x);
        auto rd   = def_reg(res);
        auto mnem = id == Annex::Base<zext> ? "ubfx" : "sbfx";
        if (ws == 64) return move(Reg{rd}, Reg{rs});
        line("{} {}, {}, #0, #{}", mnem, reg(rd, wd), reg(rs, wd), ws);
    } else if (id == Annex::Base<trunc>) {
        move(Reg{def_reg(res)}, Reg{use_reg(x)});
    } else if (id == Annex::Base<cmp>) {
        auto [a, b] = x->projs<2>();
        auto w      = width(a->type());
        auto lhs    = operand(a, w, Scratch2);
        auto rhs    = operand(a, b, w, Imm12);
        line("cmp {}, {}", lhs, rhs);
        set_flags(res);
    } else if (id == Annex::Base<cc>) {
        use_flags(x);
        if (auto r = isa_reg(res)) line("cset w{}, {}", *r, ccs[axm->sub()]);
    } else if (id == Annex::Base<csel>) {
        auto [fl, a, b] = x->projs<3>();
        use_flags(fl);
        auto w  = width(res->type());
        auto ra = operand(a, w, Scratch2);
        auto rb = operand(a, b, w, 0);
        line("csel {}, {}, {}, {}", reg(def_reg(res), w), rb, ra, ccs[axm->sub()]);
    } else {
        f->blame("{} backend: cannot emit `{}`", backend(), axm).bail();
    }
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), Target::sched_insn(app), app->arg());

    if (Axm::isa<load>(app)) {
        auto [_, p] = app->args<2>();
        auto res    = app->proj(2, 1);
        auto w      = width(res->type());
        line("ldr{} {}, [x{}]", suffix(w), reg(def_reg(res), w), use_reg(p));
        return;
    }

    if (Axm::isa<store>(app)) {
        auto [_, p, v] = app->args<3>();
        auto w         = width(v->type());
        auto src       = operand(v, w, Scratch2);
        line("str{} {}, [x{}]", suffix(w), src, use_reg(p));
        return;
    }

    app->blame("{} backend: cannot emit `{}`", backend(), app).bail();
}

void Emitter::slot_addr(size_t dst, size_t first, size_t) {
    auto off = 8 * (saved().size() + first);
    if (off <= 4095) return line("add x{}, sp, #{}", dst, off);
    line("ldr x{}, ={}", Scratch2, off);
    line("add x{}, sp, x{}", dst, Scratch2);
}

void Emitter::prologue(std::ostream& os) {
    auto frame = frame_size();
    std::println(os, "\tstp x29, x30, [sp, #-16]!");
    std::println(os, "\tmov x29, sp");
    if (frame != 0) std::println(os, "\tsub sp, sp, #{}", frame);
    for (size_t i = 0; i != saved().size(); ++i)
        std::println(os, "\tstr x{}, {}", saved()[i], at(8 * i));
}

void Emitter::epilogue() {
    for (size_t i = 0; i != saved().size(); ++i)
        line("ldr x{}, {}", saved()[i], at(8 * i));
    line("mov sp, x29");
    line("ldp x29, x30, [sp], #16");
    line("ret");
}

} // namespace

void Emit::emit(std::ostream& os, Lam* fun, bool global) { Emitter(os, *this, fun).emit(global); }

} // namespace mim::plug::arm
