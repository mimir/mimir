#include "mim/plug/x86/phase/constrain.h"

#include <ranges>

#include <mim/plug/core/core.h>

#include "mim/plug/x86/constraint.h"
#include "mim/plug/x86/liveness.h"
#include "mim/plug/x86/subst.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace {

/// The copies() of each instruction.
using Splits = fe::Vector<fe::Vector<std::pair<size_t, const Def*>>>;

Splits splits(const Liveness& live, const Liveness::Block& block) {
    auto res   = Splits(block.insns.size());
    auto after = block.live_out;
    for (auto v : live.term_uses(block))
        after.emplace(v);

    for (size_t i = block.insns.size(); i-- != 0;) {
        auto insn = block.insns[i];
        res[i]    = copies(constraint(insn), [&](const Def* v) { return after.contains(v); });
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

    auto live = Liveness(fun);
    auto all  = fe::Vector<Splits>();
    for (const auto& block : live.blocks())
        all.emplace_back(splits(live, block));
    if (std::ranges::all_of(all | std::views::join, [](const auto& s) { return s.empty(); })) return;

    auto& w  = world();
    auto map = Def2Def();
    // Blocks come in order of discovery, so a value is rebuilt before any block it dominates.
    for (size_t b = 0; b != live.blocks().size(); ++b) {
        const auto& block = live.blocks()[b];
        auto lam          = block.lam;
        auto filter       = lam->filter();
        auto term         = lam->body()->as<App>();
        auto seg          = lam;
        auto m            = core::mem_var(lam);

        auto finish = [&](const Def* body) {
            if (seg != lam)
                seg->set(w.lit_ff(), body);
            else if (body != lam->body())
                lam->unset()->set(filter, body);
        };

        for (size_t i = 0; i != block.insns.size(); ++i) {
            auto insn  = block.insns[i];
            auto subst = Subst(w, map, m);
            auto ops   = operands(insn);
            for (auto& op : ops)
                op = subst.rewrite(op);

            if (const auto& cps = all[b][i]; !cps.empty()) {
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
                    ops[j] = reg ? w.call<in>(reg, x) : x;
                }
            }

            auto new_app = rebuild(insn, m, ops);
            map[insn]    = new_app;
            m            = is_mem(new_app) ? new_app : new_app->proj(2, 0);
        }
        finish(Subst(w, map, m).rewrite(term));
    }
}

} // namespace mim::plug::x86::phase
