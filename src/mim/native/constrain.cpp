#include <ranges>

#include <mim/liveness.h>

#include <mim/plug/core/core.h>

#include "mim/native/regalloc.h"
#include "mim/native/subst.h"

namespace mim::native {

namespace {

/// Per instruction, the copies() before it and the fixed results that stay live after it.
struct Split {
    fe::Vector<std::pair<size_t, std::optional<size_t>>> pre;
    fe::Vector<std::pair<size_t, size_t>> post;
};
using Splits = fe::Vector<Split>;

Splits splits(const Liveness& live, const Liveness::Block& block) {
    const auto& target = live.target();
    auto res           = Splits(block.insns.size());
    auto after         = block.live_out;
    for (auto v : live.term_uses(block))
        after.emplace(v);

    for (size_t i = block.insns.size(); i-- != 0;) {
        auto insn  = block.insns[i];
        auto c     = target.constraint(insn);
        res[i].pre = copies(c, [&](const Def* v) { return after.contains(v); });
        for (auto [j, reg] : c.results)
            if (after.contains(Target::result(insn, j))) res[i].post.emplace_back(j, reg);
        for (auto d : live.defs(insn))
            after.erase(d);
        for (auto u : live.uses(insn))
            after.emplace(u);
    }
    return res;
}

} // namespace

void Constrain::visit(Lam* fun) {
    if (!Lam::isa_mut_returning(fun)) return;

    const auto& t = target();
    auto live     = Liveness(t, fun);
    auto all      = fe::Vector<Splits>();
    for (const auto& block : live.blocks())
        all.emplace_back(splits(live, block));
    if (std::ranges::all_of(all | std::views::join, [](const auto& s) { return s.pre.empty() && s.post.empty(); }))
        return;

    auto& w  = world();
    auto map = Def2Def();
    // Blocks come in order of discovery, so a value is rebuilt before any block it dominates.
    for (size_t b = 0; b != live.blocks().size(); ++b) {
        const auto& block = live.blocks()[b];
        auto lam          = block.lam;
        auto filter       = lam->filter();
        auto term         = lam->body()->as<App>();
        auto seg          = lam;
        auto m            = plug::core::mem_var(lam);

        auto finish = [&](const Def* body) {
            if (seg != lam)
                seg->set(w.lit_ff(), body);
            else if (body != lam->body())
                lam->unset()->set(filter, body);
        };

        for (size_t i = 0; i != block.insns.size(); ++i) {
            auto insn  = block.insns[i];
            auto subst = Subst(t, w, map, m);
            auto ops   = t.operands(insn);
            for (auto& op : ops)
                op = subst.rewrite(op);

            if (const auto& cps = all[b][i].pre; !cps.empty()) {
                auto dom  = DefVec{m->type()};
                auto args = DefVec{m};
                for (auto [j, _] : cps) {
                    dom.emplace_back(ops[j]->type());
                    args.emplace_back(ops[j]);
                }
                auto pre = w.mut_con(dom)->set(lam->dbg());
                finish(w.app(pre, w.tuple(args)));

                auto n = dom.size();
                seg    = pre;
                m      = pre->var(n, 0);
                for (size_t k = 0; auto [j, reg] : cps) {
                    auto x = pre->var(n, ++k)->set(ops[j]->sym());
                    ops[j] = reg ? t.in(Target::Gpr, *reg, x) : x;
                }
            }

            auto new_app = t.rebuild(insn, m, ops);
            map[insn]    = new_app;
            m            = t.is_mem(new_app) ? new_app : new_app->proj(2, 0);

            // A fixed result leaves its register right away, so its own live range is short.
            if (const auto& post = all[b][i].post; !post.empty()) {
                auto dom  = DefVec{m->type()};
                auto args = DefVec{m};
                for (auto [j, reg] : post) {
                    auto v = Target::result(new_app, j);
                    dom.emplace_back(v->type());
                    args.emplace_back(t.in(Target::Gpr, reg, v));
                }
                auto next = w.mut_con(dom)->set(lam->dbg());
                finish(w.app(next, w.tuple(args)));

                auto n = dom.size();
                seg    = next;
                m      = next->var(n, 0);
                for (size_t k = 0; auto [j, _] : post)
                    map[Target::result(insn, j)] = next->var(n, ++k);
            }
        }
        finish(Subst(t, w, map, m).rewrite_jump(term));
    }
}

} // namespace mim::native
