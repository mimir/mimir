#include "mim/plug/x86/liveness.h"

#include <ranges>

#include <mim/plug/math/math.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

Liveness::Liveness(Lam* fun) {
    add(fun);
    for (size_t i = 0; i != blocks_.size(); ++i)
        chain(i);
    for (auto& block : blocks_)
        values(block);

    for (size_t i = 0, e = blocks_.size(); i != e; ++i) {
        auto gen = [&](const DefVec& used) {
            for (auto value : used)
                if (&def_block(value) != &blocks_[i]) live_in(i, value);
        };
        gen(blocks_[i].term_uses);
        for (const auto& used : blocks_[i].uses)
            gen(used);
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
        m = prev;
    }
    std::ranges::reverse(insns);
    for (size_t i = 0; i != insns.size(); ++i)
        insn2block_.emplace(insns[i], std::pair(b, i));
    blocks_[b].insns = std::move(insns);

    for_each_succ(term, [&](Lam* bb) {
        auto s = add(bb);
        blocks_[b].succs.emplace_back(s);
        blocks_[s].preds.emplace_back(b);
    });
}

Liveness::Class Liveness::cls(const Def* value) {
    auto type = value->type();
    if (Axm::isa<x86::Flags>(type)) return Flags;
    if (Axm::isa<Spill>(type) || Axm::isa<FSpill>(type)) return Stack;
    return math::isa_f(type) ? Fpr : Gpr;
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

void Liveness::values(Block& block) {
    auto run = [this](auto... defs) {
        auto res  = DefVec();
        auto done = DefSet();
        (values(defs, res, done), ...);
        return res;
    };

    block.params = run(block.lam->var());
    for (auto insn : block.insns) {
        block.defs.emplace_back(run(insn));
        block.uses.emplace_back(run(insn->callee(), insn->arg()));
    }
    auto term = block.lam->body()->as<App>();
    auto ex   = term->callee()->isa<Extract>();
    if (auto cc = ex ? isa_sched_cc(ex->index()) : nullptr)
        block.term_uses = run(cc->arg(), ex->tuple(), term->arg());
    else
        block.term_uses = run(term);
}

const Liveness::Block& Liveness::def_block(const Def* value) const {
    auto root = value;
    while (auto ex = root->isa<Extract>())
        root = ex->tuple();
    if (auto var = root->isa<Var>()) return blocks_[lam2block_.find(var->binder()->as<Lam>())->second];
    return blocks_[insn2block_.find(root)->second.first];
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

    gen(block.term_uses);
    for (size_t i = block.insns.size(); i-- != 0;) {
        const auto& ds = block.defs[i];
        // A dead result occupies a register nonetheless, unless it is a condition that only a branch consumes.
        if (!is_sched_cc(block.insns[i])) gen(ds);
        kill(ds);
        gen(block.uses[i]);
    }
    gen(block.params);
}

} // namespace mim::plug::x86
