#include "mim/plug/arm/emit.h"

#include <print>
#include <ranges>
#include <sstream>

#include <mim/plugin.h>

#include "mim/plug/arm/liveness.h"

namespace mim::plug::arm {

namespace {

/// The AAPCS64 condition codes in hardware order; `arm.cc`'s subtag indexes this table.
constexpr std::array<std::string_view, 16> ccs
    = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};

/// `x17` holds a literal operand; `x16` holds a second one or breaks a copy cycle.
constexpr size_t Scratch = encoding(reg::x16), Scratch2 = encoding(reg::x17);

/// The bit width of the register holding a value of @p type.
nat_t width(const Def* type) { return Idx::expect_bitwidth(type, "an integer"); }

/// The `b`/`h` suffix of a load or store of width @p w.
std::string_view suffix(nat_t w) { return w <= 8 ? "b" : w <= 16 ? "h" : ""; }

std::string reg(size_t r, nat_t w) { return std::format("{}{}", w == 64 ? 'x' : 'w', r); }

nat_t nat(const Def* def) {
    if (auto l = Lit::isa(def)) return *l;
    def->blame(MIM_ARM_BE "expected a literal, got `{}`", def).bail();
}

/// The source of a copy: a register or a literal.
struct Src {
    size_t reg     = 0;
    const Def* imm = nullptr;
};
struct Move {
    size_t dst;
    Src src;
};
using Moves = fe::Vector<Move>;

class Emitter {
public:
    Emitter(std::ostream& os, Emit& emit, Lam* fun)
        : os_(os)
        , emit_(emit)
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

    std::string slot(size_t off) const { return std::format("[sp, #{}]", off); }
    static std::string label(Lam* lam) { return std::format(".L{}", lam->unique_name()); }
    std::string symbol(Lam*);
    void collect(const Def*, DefSet&);
    void epilogue();

    /// Reserves @p n bytes in the frame and yields the byte offset of their start.
    size_t slot_of(nat_t n) {
        auto off = frame_;
        frame_ += (n + 7) & ~nat_t(7);
        return off;
    }
    /// The register of the result @p def.
    size_t def_reg(const Def* def) const;
    /// The register of the operand @p def, an `arm.in r v`.
    size_t use_reg(const Def* def) const;
    Src source(const Def* def) const { return Lit::isa(def) ? Src{.imm = def} : Src{.reg = use_reg(def)}; }
    void move(size_t dst, size_t src);
    void mov_imm(size_t dst, const Def* lit);
    /// The register operand @p def of width @p w; a literal goes into @p scratch.
    std::string operand(const Def* def, nat_t w, size_t scratch);
    /// The second operand @p b after @p a: an immediate below @p limit, otherwise a register.
    std::string operand(const Def* a, const Def* b, nat_t w, nat_t limit);
    void use_flags(const Def* fl) const;

    void insn(const Def* res, const Def* f, const Def* x);
    void step(const App*);
    void block(const Liveness::Block&);
    void parallel(Moves moves);
    void copy(Lam* target, const Def* arg);
    void jump(Lam* target);
    void call(Lam* callee, const Def* arg);

    std::ostream& os_;
    Emit& emit_;
    Lam* fun_;
    const Def* ret_;
    Liveness live_;
    std::ostringstream body_;
    DefMap<size_t> regs_;
    size_t frame_ = 0;
    fe::Vector<size_t> saved_;
    Lam* next_        = nullptr;
    const Def* flags_ = nullptr; ///< The result of the `cmp` that the flags hold.
};

std::string Emitter::symbol(Lam* lam) { return emit_.symbol(lam); }

void Emitter::collect(const Def* def, DefSet& done) {
    if (def->isa_mut() || def->is_closed() || is_mem(def) || !done.emplace(def).second) return;
    if (Axm::isa<arm::in>(def)) {
        regs_.emplace(def->as<App>()->arg(), in_reg(def));
        return;
    }
    for (auto op : def->ops())
        collect(op, done);
}

size_t Emitter::def_reg(const Def* def) const {
    if (auto r = fe::lookup(regs_, def)) return *r;
    def->blame(MIM_ARM_BE "`{}` has no register", def).bail();
}

size_t Emitter::use_reg(const Def* def) const {
    if (Axm::isa<arm::in>(def)) return in_reg(def);
    if (Lit::isa(def)) def->blame(MIM_ARM_BE "literal `{}` cannot be an operand here", def).bail();
    def->blame(MIM_ARM_BE "`{}` is not in a register", def).bail();
}

void Emitter::move(size_t dst, size_t src) {
    if (dst != src) line("mov x{}, x{}", dst, src);
}

void Emitter::mov_imm(size_t dst, const Def* lit) { line("ldr {}, ={}", reg(dst, width(lit->type())), *Lit::isa(lit)); }

std::string Emitter::operand(const Def* def, nat_t w, size_t scratch) {
    if (!Lit::isa(def)) return reg(use_reg(def), w);
    mov_imm(scratch, def);
    return reg(scratch, w);
}

std::string Emitter::operand(const Def* a, const Def* b, nat_t w, nat_t limit) {
    if (auto l = Lit::isa(b); l && *l < limit) return std::format("#{}", *l);
    return operand(b, w, Lit::isa(a) ? Scratch : Scratch2);
}

void Emitter::use_flags(const Def* fl) const {
    if (fl != flags_) fl->blame(MIM_ARM_BE "flags `{}` are clobbered before their use", fl).bail();
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
    else if (id == Annex::Base<udiv>)
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
        if (ws == 64) return move(rd, rs);
        line("{} {}, {}, #0, #{}", mnem, reg(rd, wd), reg(rs, wd), ws);
    } else if (id == Annex::Base<trunc>) {
        move(def_reg(res), use_reg(x));
    } else if (id == Annex::Base<cmp>) {
        auto [a, b] = x->projs<2>();
        auto w      = width(a->type());
        auto lhs    = operand(a, w, Scratch2);
        auto rhs    = operand(a, b, w, Imm12);
        line("cmp {}, {}", lhs, rhs);
        flags_ = res;
    } else if (id == Annex::Base<cc>) {
        use_flags(x);
        if (auto r = fe::lookup(regs_, res)) line("cset w{}, {}", *r, ccs[axm->sub()]);
    } else if (id == Annex::Base<csel>) {
        auto [fl, a, b] = x->projs<3>();
        use_flags(fl);
        auto w  = width(res->type());
        auto ra = operand(a, w, Scratch2);
        auto rb = operand(a, b, w, 0);
        line("csel {}, {}, {}, {}", reg(def_reg(res), w), rb, ra, ccs[axm->sub()]);
    } else {
        f->blame(MIM_ARM_BE "cannot emit `{}`", axm).bail();
    }
}

void Emitter::step(const App* app) {
    if (Axm::isa<sched>(app)) return insn(app->proj(2, 1), sched_insn(app), app->arg());

    if (Axm::isa<arm::slot>(app)) {
        auto n   = app->callee()->as<App>()->arg();
        auto dst = def_reg(app->proj(2, 1));
        auto off = slot_of(nat(n));
        if (off <= 4095) {
            line("add x{}, sp, #{}", dst, off);
        } else {
            mov_imm(Scratch2, n);
            line("add x{}, sp, x{}", dst, Scratch2);
        }
        return;
    }

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

    app->blame(MIM_ARM_BE "cannot emit `{}`", app).bail();
}

/// Sequentializes the parallel copies @p moves; `x16` breaks a cycle.
void Emitter::parallel(Moves moves) {
    auto imms = Moves();
    for (auto& m : moves)
        if (m.src.imm) imms.emplace_back(m);
    std::erase_if(moves, [](const Move& m) { return m.src.imm || m.dst == m.src.reg; });

    while (!moves.empty()) {
        auto free = std::ranges::find_if(moves, [&](const Move& m) {
            return std::ranges::none_of(moves, [&](const Move& n) { return n.src.reg == m.dst; });
        });
        if (free != moves.end()) {
            move(free->dst, free->src.reg);
            moves.erase(free);
            continue;
        }
        // Only cycles are left: save one destination in the scratch register and let its readers use that.
        auto m = moves.back();
        moves.pop_back();
        line("mov x{}, x{}", Scratch, m.dst);
        for (auto& n : moves)
            if (n.src.reg == m.dst) n.src.reg = Scratch;
        move(m.dst, m.src.reg);
    }

    for (auto& m : imms)
        mov_imm(m.dst, m.src.imm);
}

void Emitter::copy(Lam* target, const Def* arg) {
    auto moves = Moves();
    auto n     = target->num_vars();
    for (size_t i = 0; i != n; ++i) {
        auto x = target->var(n, i);
        if (is_mem(x)) continue;
        if (auto r = fe::lookup(regs_, x)) moves.emplace_back(Move{*r, source(arg->proj(n, i))});
    }
    parallel(std::move(moves));
}

void Emitter::jump(Lam* target) {
    if (target != next_) line("b {}", label(target));
}

void Emitter::epilogue() {
    for (size_t i = 0; i != saved_.size(); ++i)
        line("ldr x{}, {}", saved_[i], slot(8 * i));
    line("mov sp, x29");
    line("ldp x29, x30, [sp], #16");
    line("ret");
}

void Emitter::call(Lam* callee, const Def* arg) {
    auto n     = arg->num_projs();
    auto moves = Moves();
    auto c     = size_t(0);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (auto x = arg->proj(n, i); !is_mem(x)) {
            if (c == Arg_Regs.size()) arg->blame(MIM_ARM_BE "too many arguments are not supported yet").bail();
            moves.emplace_back(Move{Arg_Regs[c++], source(x)});
        }
    }
    parallel(std::move(moves));
    line("bl {}", symbol(callee));

    auto ret = arg->proj(n, n - 1);
    if (ret == ret_) return epilogue();

    auto k    = Lam::isa_mut_basicblock(ret);
    auto m    = k->num_vars();
    auto back = Moves();
    for (size_t i = 0; i != m; ++i)
        if (auto x = k->var(m, i); !is_mem(x))
            if (auto r = fe::lookup(regs_, x)) back.emplace_back(Move{*r, Src{.reg = Ret_Reg}});
    parallel(std::move(back));
    jump(k);
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
            moves.emplace_back(Move{Ret_Reg, source(a)});
        }
        if (moves.size() > 1) b.lam->blame(MIM_ARM_BE "`{}` returns more than one value", b.lam).bail();
        parallel(std::move(moves));
        epilogue();
    } else if (auto f = Lam::isa_mut_returning(callee)) {
        call(f, arg);
    } else if (auto ex = callee->isa<Extract>()) {
        auto targets = ex->tuple();
        if (targets->num_projs() != 2) b.lam->blame(MIM_ARM_BE "only two-way branches are supported yet").bail();
        auto [e, t] = targets->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) b.lam->blame(MIM_ARM_BE "`{}` branches to something other than blocks", b.lam).bail();
        auto c          = ex->index();
        auto trampoline = takes_values(target(), t);
        auto t_lbl      = trampoline ? label(b.lam) + ".t" : label(t);
        if (auto app = target().isa_sched_cc(c)) {
            use_flags(app->arg());
            line("b.{} {}", ccs[Axm::as<cc, 1>(sched_insn(app)).sub()], t_lbl);
        } else if (Lit::isa(c)) {
            c->blame(MIM_ARM_BE "a branch on the literal `{}`", c).bail();
        } else {
            line("cbnz w{}, {}", use_reg(c), t_lbl);
        }
        copy(e, arg);
        if (trampoline) {
            line("b {}", label(e));
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
        b.lam->blame(MIM_ARM_BE "cannot emit the jump to `{}`", callee).bail();
    }
}

void Emitter::emit(bool global) {
    if (!ret_) fun_->blame(MIM_ARM_BE "`{}` does not end in a return continuation", fun_).bail();

    auto done = DefSet();
    for (const auto& b : live_.blocks()) {
        for (auto insn : b.insns) {
            collect(insn->callee(), done);
            collect(insn->arg(), done);
        }
        collect(b.lam->body(), done);
    }
    auto used = std::array<bool, Num_Regs>{};
    for (auto [def, r] : regs_)
        used[r] = true;
    for (size_t r = 0; r != Num_Regs; ++r)
        if (!Caller_Saved[r] && !reserved(r) && used[r]) saved_.emplace_back(r);
    frame_ = 8 * saved_.size();

    auto order = layout(live_);

    auto n     = fun_->num_vars();
    auto moves = Moves();
    for (size_t i = 0, a = 0; i + 1 < n; ++i) {
        if (auto x = fun_->var(n, i); !is_mem(x)) {
            if (a == Arg_Regs.size()) fun_->blame(MIM_ARM_BE "too many arguments are not supported yet").bail();
            if (auto r = fe::lookup(regs_, x)) moves.emplace_back(Move{*r, Src{.reg = Arg_Regs[a]}});
            ++a;
        }
    }
    parallel(std::move(moves));

    for (size_t i = 0; i != order.size(); ++i) {
        auto b = order[i];
        next_  = i + 1 != order.size() ? order[i + 1]->lam : nullptr;
        if (i != 0) std::println(body_, "{}:", label(b->lam));
        flags_ = nullptr;
        block(*b);
    }

    auto sym   = symbol(fun_);
    auto frame = (frame_ + 15) & ~nat_t(15);
    if (global) std::println(os_, "\t.globl {}", sym);
    std::println(os_, "\t.type {}, %function", sym);
    std::println(os_, "{}:", sym);
    std::println(os_, "\tstp x29, x30, [sp, #-16]!");
    std::println(os_, "\tmov x29, sp");
    if (frame != 0) std::println(os_, "\tsub sp, sp, #{}", frame);
    for (size_t i = 0; i != saved_.size(); ++i)
        std::println(os_, "\tstr x{}, {}", saved_[i], slot(8 * i));
    os_ << body_.view();
    std::println(os_, "\t.size {}, .-{}", sym, sym);
}

} // namespace

void Emit::emit(std::ostream& os, Lam* fun, bool global) { Emitter(os, *this, fun).emit(global); }

} // namespace mim::plug::arm
