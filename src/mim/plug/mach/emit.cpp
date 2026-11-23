#include "mim/plug/mach/emit.h"

#include <ranges>

#include "mim/plug/mach/liveness.h"

namespace mim::plug::mach {

void Emitter::collect(const Def* def, DefSet& done) {
    if (def->isa_mut() || def->is_closed() || Target::is_mem(def) || !done.emplace(def).second) return;
    if (Target::is_in(def)) {
        regs_.emplace(Target::strip(def), Target::in_reg(def));
        return;
    }
    for (auto op : def->ops())
        collect(op, done);
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
        if (Target::is_mem(x)) continue;
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
        if (auto x = arg->proj(n, i); !Target::is_mem(x)) {
            auto cls = target_.cls(Target::strip(x));
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
        if (auto x = k->var(m, i); !Target::is_mem(x)) {
            auto cls = target_.cls(x);
            if (auto r = fe::lookup(regs_, x)) back.emplace_back(Reg{*r, cls}, Src{.reg = {target_.ret_reg(cls), cls}});
        }
    parallel(std::move(back), {});
    jump(k);
}

void Emitter::insn(const App* app) {
    if (Axm::isa<sched>(app)) {
        auto f   = Target::sched_insn(app);
        auto res = app->proj(2, 1);
        if (Axm::isa<mach::spill, 1>(f)) return spill(source(app->arg()), app->arg()->type(), spill_slot(res));
        if (Axm::isa<mach::reload, 1>(f))
            return reload(Reg{def_reg(res), target_.cls(res)}, res->type(), spill_slot(app->arg()));
    } else if (Axm::isa<slot>(app)) {
        auto first = alloc_slots(nat(app->callee()->as<App>()->arg()));
        return slot_addr(def_reg(app->proj(2, 1)), first, num_slots_ - first);
    }
    step(app);
}

void Emitter::block(Lam* lam, const fe::Vector<const App*>& insns) {
    for (auto app : insns)
        insn(app);

    auto term   = lam->body()->as<App>();
    auto arg    = term->arg();
    auto callee = term->callee();
    if (callee == ret_) {
        auto moves = Moves();
        for (auto a : term->args()) {
            if (Target::is_mem(a)) continue;
            auto cls = target_.cls(Target::strip(a));
            moves.emplace_back(Reg{target_.ret_reg(cls), cls}, source(a));
        }
        if (moves.size() > 1) lam->blame("{} backend: `{}` returns more than one value", backend(), lam).bail();
        parallel(std::move(moves), {});
        epilogue();
    } else if (auto f = Lam::isa_mut_returning(callee)) {
        call(f, arg);
    } else if (auto ex = callee->isa<Extract>()) {
        auto targets = ex->tuple();
        if (targets->num_projs() != 2)
            lam->blame("{} backend: only two-way branches are supported yet", backend()).bail();
        auto [e, t] = targets->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) lam->blame("{} backend: `{}` branches to something other than blocks", backend(), lam).bail();
        auto c          = ex->index();
        auto trampoline = takes_values(t);
        auto t_lbl      = trampoline ? label(lam) + ".t" : label(t);
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
        lam->blame("{} backend: cannot emit the jump to `{}`", backend(), callee).bail();
    }
}

void Emitter::run(bool global) {
    auto live = Liveness(target_, fun_);
    if (!ret_) fun_->blame("{} backend: `{}` does not end in a return continuation", backend(), fun_).bail();

    auto done = DefSet();
    for (const auto& b : live.blocks()) {
        for (auto insn : b.insns) {
            collect(insn->callee(), done);
            collect(insn->arg(), done);
            for (auto [j, r] : target_.constraint(insn).results)
                regs_.emplace(Target::result(insn, j), r);
        }
        collect(b.lam->body(), done);
    }
    auto used = std::array<fe::Vector<bool>, Target::Num_Classes>{};
    for (size_t c = 0; c != Target::Num_Classes; ++c)
        used[c].resize(target_.num_regs(Target::Class(c)));
    for (auto [def, r] : regs_)
        if (auto& u = used[target_.cls(def)]; r < u.size()) u[r] = true;
    for (size_t c = 0; c != Target::Num_Classes; ++c) {
        auto cls = Target::Class(c);
        for (size_t r = 0; r != used[c].size(); ++r)
            if (used[c][r] && !target_.caller_saved(cls, r) && !target_.reserved(cls, r)) saved_[c].emplace_back(r);
    }

    auto order = layout(live);

    auto n     = fun_->num_vars();
    auto moves = Moves();
    auto num   = std::array<size_t, Target::Num_Classes>{};
    for (size_t i = 0; i + 1 < n; ++i) {
        if (auto x = fun_->var(n, i); !Target::is_mem(x)) {
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
        block(b->lam, b->insns);
    }

    auto sym = symbol(fun_);
    if (global) std::println(os_, "\t.globl {}", sym);
    if (Target::is_elf()) std::println(os_, "\t.type {}, {}", sym, function_type());
    std::println(os_, "{}:", sym);
    prologue(os_);
    os_ << body_.view();
    if (Target::is_elf()) std::println(os_, "\t.size {}, .-{}", sym, sym);
}

void mim_mach_emit(Emitter& emitter, bool global) { emitter.run(global); }

} // namespace mim::plug::mach
