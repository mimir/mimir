#include "mim/plug/mach/liveness.h"

#include <deque>
#include <ranges>

#include <mim/world.h>

namespace mim::plug::mach {

Liveness::Liveness(const Target& target, Lam* fun)
    : target_(&target) {
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
    if (!term) lam->blame("{} backend: `{}` does not end in a jump", target_->name(), lam).bail();

    const Def* end = nullptr;
    for (auto arg : term->args())
        if (target_->is_mem(arg)) end = arg;
    if (!end) lam->blame("{} backend: `{}` has no `mem.M` chain", target_->name(), lam).bail();

    auto insns = fe::Vector<const App*>();
    for (auto m = end; !(m->isa<Extract>() && m->as<Extract>()->tuple() == lam->var()) && m != lam->var();) {
        auto node = m->isa<Extract>() ? m->as<Extract>()->tuple() : m;
        auto app  = node->isa<App>();
        auto prev = app ? target_->prev_mem(app) : nullptr;
        if (!prev) m->blame("{} backend: unexpected memory `{}` in `{}`", target_->name(), m, lam).bail();
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
        if (target_->is_mem(def) || def->type()->isa<Pi>()) return;
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
    if (auto cc = ex ? target_->isa_sched_cc(ex->index()) : nullptr)
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
        ++count[target_->cls(value)];

    auto gen = [&](const DefVec& values) {
        for (auto value : values)
            if (live.emplace(value).second) ++count[target_->cls(value)];
        for (size_t c = 0; c != Num_Classes; ++c)
            block.max_live[c] = std::max(block.max_live[c], count[c]);
    };
    auto kill = [&](const DefVec& values) {
        for (auto value : values)
            if (live.erase(value)) --count[target_->cls(value)];
    };

    gen(block.term_uses);
    for (size_t i = block.insns.size(); i-- != 0;) {
        const auto& ds = block.defs[i];
        // A dead result occupies a register nonetheless, unless it is a condition that only a branch consumes.
        if (!target_->is_sched_cc(block.insns[i])) gen(ds);
        kill(ds);
        gen(block.uses[i]);
    }
    gen(block.params);
}

namespace {

/// The block that @p block should fall through to; @p ret is the return continuation of its function.
Lam* fall_through(const Liveness::Block& block, const Def* ret) {
    auto term   = block.lam->body()->as<App>();
    auto callee = term->callee();
    if (callee == ret) return nullptr;
    if (Lam::isa_mut_returning(callee)) {
        auto k = term->arg(term->num_args(), term->num_args() - 1);
        return k == ret ? nullptr : Lam::isa_mut_basicblock(k);
    }
    if (auto ex = callee->isa<Extract>()) {
        if (ex->tuple()->num_projs() != 2) return nullptr;
        auto [e, t] = ex->tuple()->projs<2>([](const Def* d) { return Lam::isa_mut_basicblock(d); });
        if (!e || !t) return nullptr;
        return takes_values(t) ? t : e;
    }
    return Lam::isa_mut_basicblock(callee);
}

} // namespace

bool takes_values(Lam* lam) {
    return std::ranges::any_of(lam->vars(), [](const Def* x) { return !Target::is_mem(x); });
}

fe::Vector<const Liveness::Block*> layout(const Liveness& live) {
    auto ret    = live.blocks().front().lam->ret_var();
    auto order  = fe::Vector<const Liveness::Block*>();
    auto placed = LamSet();
    for (auto queue = std::deque<Lam*>{live.blocks().front().lam}; !queue.empty(); queue.pop_front()) {
        for (auto lam = queue.front(); lam && placed.emplace(lam).second;) {
            auto b = live.block(lam);
            order.emplace_back(b);
            for (auto s : b->succs)
                queue.emplace_back(live.blocks()[s].lam);
            lam = fall_through(*b, ret);
            if (lam && !live.block(lam)) lam = nullptr;
        }
    }
    return order;
}

void rewrite_blocks(const Liveness& live, Rewriter& rw) {
    for (const auto& block : live.blocks()) {
        auto lam    = block.lam;
        auto filter = rw.rewrite(lam->filter());
        auto body   = rw.rewrite(lam->body());
        lam->unset()->set(filter, body);
    }
}

const Def* Assign::rewrite(const Def* def) {
    auto& w = world();
    if (def->isa_mut() || def->is_closed()) return def;
    if (live_.target().is_in(def)) {
        auto app = def->as<App>();
        return w.app(rewrite(app->callee()), raw(app->arg()));
    }
    if (live_.local(def)) {
        if (auto r = fe::lookup(regs_, def)) return live_.target().in(cls_, *r, raw(def));
        // A tuple of values, e.g. the whole variable of a block or an instruction, is used through its projections.
        if (auto projs = def->projs(); projs.size() > 1 && std::ranges::any_of(projs, [&](const Def* d) {
                                           return regs_.contains(d) || d->num_projs() > 1;
                                       })) {
            auto ops = DefVec();
            for (auto d : projs)
                ops.emplace_back(rewrite(d));
            return w.tuple(ops);
        }
        return raw(def);
    }
    return Rewriter::rewrite(def);
}

const Def* Assign::raw(const Def* def) {
    if (auto ex = def->isa<Extract>()) return world().extract(raw(ex->tuple()), ex->index());
    if (def->isa<Var>()) return def;
    return Rewriter::rewrite(def);
}

} // namespace mim::plug::mach
