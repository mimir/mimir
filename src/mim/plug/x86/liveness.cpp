#include "mim/plug/x86/liveness.h"

#include <ranges>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

Liveness::Liveness(Lam* fun) {
    add(fun);
    for (size_t i = 0; i != blocks_.size(); ++i)
        chain(i);

    for (size_t i = 0, e = blocks_.size(); i != e; ++i) {
        auto used = term_uses(blocks_[i]);
        for (auto insn : blocks_[i].insns)
            std::ranges::copy(uses(insn), std::back_inserter(used));
        for (auto value : used)
            if (&def_block(value) != &blocks_[i]) live_in(i, value);
    }

    for (auto& block : blocks_)
        sweep(block);
}

size_t Liveness::add(Lam* lam) {
    auto [i, ins] = lam2block_.emplace(lam, blocks_.size());
    if (ins) blocks_.emplace_back(lam);
    return i->second;
}

void Liveness::chain(size_t b) {
    auto lam  = blocks_[b].lam;
    auto term = lam->body()->isa<App>();
    if (!term) lam->blame(MIM_X86_BE "`{}` does not end in a jump", lam).bail();

    const Def* end = nullptr;
    for (auto arg : term->args())
        if (is_mem(arg)) end = arg;
    if (!end) lam->blame(MIM_X86_BE "`{}` has no `mem.M` chain", lam).bail();

    auto insns = fe::Vector<const App*>();
    for (auto m = end; !(m->isa<Extract>() && m->as<Extract>()->tuple() == lam->var()) && m != lam->var();) {
        auto node = m->isa<Extract>() ? m->as<Extract>()->tuple() : m;
        auto app  = node->isa<App>();
        auto prev = app ? prev_mem(app) : nullptr;
        if (!prev) m->blame(MIM_X86_BE "unexpected memory `{}` in `{}`", m, lam).bail();
        insns.emplace_back(app);
        insn2block_.emplace(app, b);
        m = prev;
    }
    std::ranges::reverse(insns);
    blocks_[b].insns = std::move(insns);

    auto succ = [&](const Def* def) {
        if (auto bb = Lam::isa_mut_basicblock(def); bb && bb->is_set()) {
            auto s = add(bb);
            blocks_[b].succs.emplace_back(s);
            blocks_[s].preds.emplace_back(b);
        }
    };
    succ(term->callee());
    if (auto ex = term->callee()->isa<Extract>())
        for (auto target : ex->tuple()->projs())
            succ(target);
    for (auto arg : term->args())
        succ(arg);
}

Liveness::Class Liveness::cls(const Def* value) {
    auto type = value->type();
    return Axm::isa<Fl>(type) ? Flags : Axm::isa<Spill>(type) ? Stack : Gpr;
}

bool Liveness::local(const Def* def) const {
    auto root = def;
    while (auto ex = root->isa<Extract>()) {
        if (!Lit::isa(ex->index())) break;
        root = ex->tuple();
    }
    auto var = root->isa<Var>();
    return var ? lam2block_.contains(var->binder()->isa<Lam>()) : insn2block_.contains(root);
}

void Liveness::values(const Def* def, DefVec& res, DefSet& done) const {
    if (def->isa_mut() || !done.emplace(def).second) return;

    if (local(def)) {
        if (is_mem(def) || def->type()->isa<Pi>()) return;
        if (auto n = def->num_projs(); n == 1)
            res.emplace_back(def);
        else
            for (size_t i = 0; i != n; ++i)
                values(def->proj(n, i), res, done);
    } else {
        for (auto op : def->ops())
            values(op, res, done);
    }
}

DefVec Liveness::params(const Block& block) const {
    auto res  = DefVec();
    auto done = DefSet();
    values(block.lam->var(), res, done);
    return res;
}

DefVec Liveness::defs(const App* insn) const {
    auto res  = DefVec();
    auto done = DefSet();
    values(insn, res, done);
    return res;
}

DefVec Liveness::uses(const App* insn) const {
    auto res  = DefVec();
    auto done = DefSet();
    values(insn->callee(), res, done);
    values(insn->arg(), res, done);
    return res;
}

DefVec Liveness::term_uses(const Block& block) const {
    auto res  = DefVec();
    auto done = DefSet();
    auto term = block.lam->body()->as<App>();
    if (auto ex = term->callee()->isa<Extract>()) {
        if (auto cc = isa_sched_cc(ex->index())) {
            values(cc->arg(), res, done);
            values(ex->tuple(), res, done);
            values(term->arg(), res, done);
            return res;
        }
    }
    values(term, res, done);
    return res;
}

const Liveness::Block& Liveness::def_block(const Def* value) const {
    auto root = value;
    while (auto ex = root->isa<Extract>())
        root = ex->tuple();
    if (auto var = root->isa<Var>()) return blocks_[lam2block_.find(var->binder()->as<Lam>())->second];
    return blocks_[insn2block_.find(root)->second];
}

void Liveness::live_in(size_t b, const Def* value) {
    auto def   = size_t(&def_block(value) - blocks_.data());
    auto stack = fe::Vector<size_t>{b};
    while (!stack.empty()) {
        auto i = stack.back();
        stack.pop_back();
        if (!blocks_[i].live_in.emplace(value).second) continue;
        for (auto p : blocks_[i].preds)
            if (blocks_[p].live_out.emplace(value).second && p != def) stack.emplace_back(p);
    }
}

void Liveness::sweep(Block& block) {
    auto live  = block.live_out;
    auto count = Pressure{};
    for (auto value : live)
        ++count[cls(value)];

    auto gen = [&](const DefVec& values) {
        for (auto value : values)
            if (live.emplace(value).second) ++count[cls(value)];
        for (size_t c = 0; c != Num_Classes; ++c)
            block.max_live[c] = std::max(block.max_live[c], count[c]);
    };
    auto kill = [&](const DefVec& values) {
        for (auto value : values)
            if (live.erase(value)) --count[cls(value)];
    };

    gen(term_uses(block));
    for (auto insn : block.insns | std::views::reverse) {
        auto ds = defs(insn);
        // A dead result occupies a register nonetheless, unless it is a condition that only a branch consumes.
        if (!is_sched_cc(insn)) gen(ds);
        kill(ds);
        gen(uses(insn));
    }
    gen(params(block));
}

} // namespace mim::plug::x86
