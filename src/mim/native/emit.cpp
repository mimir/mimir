#include "mim/native/emit.h"

#include <ranges>

namespace mim::native {

nat_t Emitter::nat(const Def* def) const {
    if (auto l = Lit::isa(def)) return *l;
    def->blame("{} backend: expected a literal, got `{}`", backend(), def).bail();
}

void Emitter::collect(const Def* def, DefSet& done) {
    if (def->isa_mut() || def->is_closed() || target_.is_mem(def) || !done.emplace(def).second) return;
    if (target_.is_in(def)) {
        regs_.emplace(target_.strip(def), Target::in_reg(def));
        return;
    }
    for (auto op : def->ops())
        collect(op, done);
}

size_t Emitter::spill_slot(const Def* def) {
    auto [i, ins] = slots_.emplace(def, num_slots_);
    if (ins) ++num_slots_;
    return i->second;
}

size_t Emitter::def_reg(const Def* def) const {
    if (auto r = fe::lookup(regs_, def)) return *r;
    def->blame("{} backend: `{}` has no register", backend(), def).bail();
}

size_t Emitter::use_reg(const Def* def) const {
    if (target_.is_in(def)) return Target::in_reg(def);
    if (Lit::isa(def)) def->blame("{} backend: literal `{}` cannot be an operand here", backend(), def).bail();
    def->blame("{} backend: `{}` is not in a register", backend(), def).bail();
}

void Emitter::use_flags(const Def* fl) const {
    if (fl != flags_) fl->blame("{} backend: flags `{}` are clobbered before their use", backend(), fl).bail();
}

/// Sequentializes the parallel copies @p moves between registers and @p slot_moves between spill slots.
void Emitter::parallel(Moves moves, fe::Vector<std::pair<size_t, size_t>> slot_moves) {
    std::erase_if(slot_moves, [](auto m) { return m.first == m.second; });
    for (auto [_, from] : slot_moves)
        push_slot(from);
    for (auto [to, _] : slot_moves | std::views::reverse)
        pop_slot(to);

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
        if (auto tmp = scratch(d.cls)) {
            move(*tmp, d);
            for (auto& [_, from] : moves)
                if (from.reg == d) from.reg = *tmp;
            move(d, s.reg);
        } else {
            swap(s.reg, d);
            for (auto& [_, from] : moves)
                if (from.reg == d)
                    from.reg = s.reg;
                else if (from.reg == s.reg)
                    from.reg = d;
            std::erase_if(moves, [](const auto& m) { return m.first == m.second.reg; });
        }
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
        if (target_.is_mem(x)) continue;
        if (auto cls = target_.cls(x); cls == Target::Stack)
            slot_moves.emplace_back(spill_slot(x), spill_slot(a));
        else if (auto r = fe::lookup(regs_, x))
            moves.emplace_back(Reg{*r, cls}, source(a));
    }
    parallel(std::move(moves), std::move(slot_moves));
}

void Emitter::jump(Lam* target) {
    if (target != next_) jmp(label(target));
}

void Emitter::call(Lam* callee, const Def* arg) {
    auto n     = arg->num_projs();
    auto moves = Moves();
    auto num   = std::array<size_t, Target::Num_Classes>{};
    for (size_t i = 0; i + 1 < n; ++i) {
        if (auto x = arg->proj(n, i); !target_.is_mem(x)) {
            auto cls = target_.cls(target_.strip(x));
            auto& c  = num[cls];
            if (c == target_.num_arg_regs(cls))
                arg->blame("{} backend: too many arguments are not supported yet", backend()).bail();
            moves.emplace_back(Reg{target_.arg_reg(cls, c++), cls}, source(x));
        }
    }
    parallel(std::move(moves), {});
    call_insn(symbol(callee), num);

    auto ret = arg->proj(n, n - 1);
    if (ret == ret_) return epilogue();

    auto k    = Lam::isa_mut_basicblock(ret);
    auto m    = k->num_vars();
    auto back = Moves();
    for (size_t i = 0; i != m; ++i)
        if (auto x = k->var(m, i); !target_.is_mem(x)) {
            auto cls = target_.cls(x);
            if (auto r = fe::lookup(regs_, x)) back.emplace_back(Reg{*r, cls}, Src{.reg = {target_.ret_reg(cls), cls}});
        }
    parallel(std::move(back), {});
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
            if (target_.is_mem(a)) continue;
            auto cls = target_.cls(target_.strip(a));
            moves.emplace_back(Reg{target_.ret_reg(cls), cls}, source(a));
        }
        if (moves.size() > 1) b.lam->blame("{} backend: `{}` returns more than one value", backend(), b.lam).bail();
        parallel(std::move(moves), {});
        epilogue();
    } else if (auto f = Lam::isa_mut_returning(callee)) {
        call(f, arg);
    } else if (auto ex = callee->isa<Extract>()) {
        auto targets = ex->tuple();
        if (targets->num_projs() != 2)
            b.lam->blame("{} backend: only two-way branches are supported yet", backend()).bail();
        auto [e, t] = targets->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) b.lam->blame("{} backend: `{}` branches to something other than blocks", backend(), b.lam).bail();
        auto c          = ex->index();
        auto trampoline = takes_values(target_, t);
        auto t_lbl      = trampoline ? label(b.lam) + ".t" : label(t);
        if (auto app = target_.isa_sched_cc(c)) {
            use_flags(app->arg());
            branch_cc(std::get<0>(Axm::get(Target::sched_insn(app)))->sub(), t_lbl);
        } else if (Lit::isa(c)) {
            c->blame("{} backend: a branch on the literal `{}`", backend(), c).bail();
        } else {
            branch_if(use_reg(c), t_lbl);
        }
        copy(e, arg);
        if (trampoline) {
            jmp(label(e));
            label_here(t_lbl);
            copy(t, arg);
            jump(t);
        } else {
            jump(e);
        }
    } else if (auto bb = Lam::isa_mut_basicblock(callee)) {
        copy(bb, arg);
        jump(bb);
    } else {
        b.lam->blame("{} backend: cannot emit the jump to `{}`", backend(), callee).bail();
    }
}

void Emitter::emit(bool global) {
    if (!ret_) fun_->blame("{} backend: `{}` does not end in a return continuation", backend(), fun_).bail();

    auto done = DefSet();
    for (const auto& b : live_.blocks()) {
        for (auto insn : b.insns) {
            collect(insn->callee(), done);
            collect(insn->arg(), done);
            for (auto [j, r] : target_.constraint(insn).results)
                regs_.emplace(Target::result(insn, j), r);
        }
        collect(b.lam->body(), done);
    }
    auto used = fe::Vector<bool>(target_.num_regs(Target::Gpr), false);
    for (auto [def, r] : regs_)
        if (target_.cls(def) == Target::Gpr) used[r] = true;
    for (size_t r = 0; r != used.size(); ++r)
        if (!target_.caller_saved(Target::Gpr, r) && !target_.reserved(Target::Gpr, r) && used[r])
            saved_.emplace_back(r);

    auto order = layout(live_);

    auto n     = fun_->num_vars();
    auto moves = Moves();
    auto num   = std::array<size_t, Target::Num_Classes>{};
    for (size_t i = 0; i + 1 < n; ++i) {
        if (auto x = fun_->var(n, i); !target_.is_mem(x)) {
            auto cls = target_.cls(x);
            auto& c  = num[cls];
            if (c == target_.num_arg_regs(cls))
                fun_->blame("{} backend: too many arguments are not supported yet", backend()).bail();
            if (auto r = fe::lookup(regs_, x))
                moves.emplace_back(Reg{*r, cls}, Src{.reg = {target_.arg_reg(cls, c), cls}});
            ++c;
        }
    }
    parallel(std::move(moves), {});

    for (size_t i = 0; i != order.size(); ++i) {
        auto b = order[i];
        next_  = i + 1 != order.size() ? order[i + 1]->lam : nullptr;
        if (i != 0) label_here(label(b->lam));
        flags_ = nullptr;
        block(*b);
    }

    auto sym = symbol(fun_);
    if (global) std::println(os_, "\t.globl {}", sym);
    std::println(os_, "\t.type {}, {}", sym, function_type());
    std::println(os_, "{}:", sym);
    prologue(os_);
    os_ << body_.view();
    std::println(os_, "\t.size {}, .-{}", sym, sym);
}

} // namespace mim::native
