#include "mim/plug/x86/phase/isel.h"

#include <array>
#include <optional>
#include <print>

#include <pbqp/dot.h>
#include <pbqp/solver.h>

#include <mim/plugin.h>

#include <mim/plug/core/core.h>
#include <mim/plug/ll/autogen.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/imm.h"
#include "mim/plug/x86/isel_rules.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

/// @p def without the casts between pointer types around it, which `mem.legalize_ptr` inserts and which are no-ops.
const Def* strip(const Def* def) {
    while (auto cast = Axm::isa<core::bitcast>(def)) {
        if (!Axm::isa<core::Ptr>(def->type()) || !Axm::isa<core::Ptr>(cast->arg()->type())) break;
        def = cast->arg();
    }
    return def;
}

/// Does the old_world() or new_world() literal @p lit fit a sign-extended 32-bit immediate?
bool fits_imm(const Def* lit) { return x86::fits_imm(Idx::isa_lit(lit->type()), *Lit::isa(lit)); }

bool is_lowered_plugin(const Axm* axm) {
    auto p = axm->plugin();
    return p == core::Plugin_Id || p == mem::Plugin_Id || p == math::Plugin_Id;
}

/// Is @p def a memory operation, whose value is its second result?
bool mem_valued(const Def* def) {
    auto t = def->type();
    return t->num_projs() == 2 && Axm::isa<core::M>(t->proj(2, 0));
}

/// The continuation whose `mem.M` chain @p m is on.
Lam* chain(const Def* m) {
    for (;;) {
        if (auto ex = m->isa<Extract>()) m = ex->tuple();
        if (auto var = m->isa<Var>()) return var->binder()->isa<Lam>();
        auto app = m->isa<App>();
        if (!app) return nullptr;
        auto arg  = app->arg();
        auto n    = arg->num_projs();
        auto next = (const Def*)nullptr;
        for (size_t i = 0; i != n && !next; ++i)
            if (auto a = arg->proj(n, i); Axm::isa<core::M>(a->type())) next = a;
        if (!next) return nullptr;
        m = next;
    }
}

/// Does @p def depend on @p target without going through a mutable?
bool depends(const Def* def, const Def* target, DefSet& done) {
    if (def == target) return true;
    if (def->isa_mut() || !done.emplace(def).second) return false;
    for (auto op : def->ops())
        if (depends(op, target, done)) return true;
    return false;
}

/// The root axioms of the rules of `x86_isel`.
const auto& gen_roots() {
    static const auto roots = [] {
        auto res = ankerl::unordered_dense::set<flags_t>();
        for (const auto& r : rules::rewrites)
            res.emplace(r.root);
        return res;
    }();
    return roots;
}

/// The root of the rules covering @p def: its axiom, or `0` for an Extract at a dynamic index.
std::optional<flags_t> root_of(const Def* def) {
    if (strip(def) != def) return {};
    if (auto ex = def->isa<Extract>()) return Lit::isa(ex->index()) ? std::nullopt : std::optional<flags_t>(0);
    auto [axm, curry, _] = Axm::get(def);
    if (!axm || curry) return {};
    if (gen_roots().contains(axm->flags())) return axm->flags();
    return {};
}

/*
 * Costs
 */

constexpr pbqp::Cost Inf = pbqp::Cost::inf();

#define X(_) +1
constexpr size_t Num_Tys = 0 MIM_X86_RULE_TYS(X);
#undef X

/// The cheapest chains of conversion rules between two non-terminals: their costs and each chain's first conversion.
struct Chains {
    std::array<std::array<pbqp::Cost, Num_Tys>, Num_Tys> cost;
    std::array<std::array<const rules::Conversion*, Num_Tys>, Num_Tys> first{};

    Chains() {
        for (size_t i = 0; i != Num_Tys; ++i)
            for (size_t j = 0; j != Num_Tys; ++j)
                cost[i][j] = i == j ? pbqp::Cost(0) : Inf;
        for (const auto& c : rules::conversions)
            if (auto& d = cost[size_t(c.from)][size_t(c.to)]; pbqp::Cost(c.cost) < d) {
                d                                   = pbqp::Cost(c.cost);
                first[size_t(c.from)][size_t(c.to)] = &c;
            }
        for (size_t k = 0; k != Num_Tys; ++k)
            for (size_t i = 0; i != Num_Tys; ++i)
                for (size_t j = 0; j != Num_Tys; ++j)
                    if (auto c = cost[i][k] + cost[k][j]; c < cost[i][j]) cost[i][j] = c, first[i][j] = first[i][k];
    }
};

const Chains& chains() {
    static const auto res = Chains();
    return res;
}

/// Does a value of non-terminal @p ty fill a hole of type @p want as it is?
bool fills(Ty ty, Want want) {
    switch (want) {
        case Want::Reg: return ty == Ty::Reg;
        case Want::Base: return ty == Ty::Base;
        case Want::Index: return ty == Ty::Index;
        case Want::Addr: return ty == Ty::Base || ty == Ty::Index;
        case Want::Scaled: return ty == Ty::Scaled;
        default: return false;
    }
}

/// The non-terminal that @p from converts to most cheaply for a hole of type @p want, and the cost.
std::pair<Ty, pbqp::Cost> target(Ty from, Want want) {
    auto res = std::pair(from, Inf);
    for (size_t t = 0; t != Num_Tys; ++t)
        if (auto c = chains().cost[size_t(from)][t]; fills(Ty(t), want) && c < res.second) res = {Ty(t), c};
    return res;
}

/// Cost of converting a replacement or operand of non-terminal @p from into a hole of type @p want.
pbqp::Cost conv(Ty from, Want want) {
    if (is_literal(want)) return from == Ty::Lit ? 0 : Inf;
    return target(from, want).second;
}

/// Cost of the operand @p def, which is no node, in a hole of type @p want.
pbqp::Cost leaf(const Def* def, Want want) { return conv(Lit::isa(def) ? Ty::Lit : Ty::Reg, want); }

/// Does @p def satisfy a hole of type @p want that requires a literal?
bool literal(const Def* def, Want want) { return Lit::isa(def) && (want == Want::Sh || fits_imm(def)); }

/*
 * Rules
 */

Want want_of(const rules::Rewrite& r, size_t i) { return rules::wants[r.wants + i]; }

/// Is a meta variable that @p w wants bound to its rewrite rather than an operand?
bool is_param(Want w) { return w == Want::Param || w == Want::Lit; }

/// An atomic rule has no inner nodes and only holes that every non-terminal converts into.
bool atomic(const rules::Rewrite& r) {
    if (r.match->num_covers != 0) return false;
    for (size_t i = 0; i != r.num_metas; ++i)
        if (auto w = want_of(r, i); !is_param(w))
            for (size_t t = 0; t != Num_Tys; ++t)
                if (conv(Ty(t), w).is_inf()) return false;
    return true;
}

#ifdef MIM_ENABLE_CHECKS
bool has_atomic(flags_t root) {
    auto is = [root](const rules::Rewrite& r) { return r.root == root && atomic(r); };
    return std::ranges::any_of(rules::rewrites, is);
}
#endif

} // namespace

void ISel::append(const rules::Rewrite& rule, const Def* root, Alts& alts) {
    auto swaps = rule.swaps;
    // Ascending subsets of `swaps`.
    for (u32 swap = 0;; swap = ((swap | ~swaps) + 1) & swaps) {
        append(rule, root, swap, alts);
        if (swap == swaps) break;
    }
}

void ISel::append(const rules::Rewrite& r, const Def* root, u32 swap, Alts& alts) {
    const auto& m = *r.match;
    auto metas    = DefVec(r.num_metas, [](size_t) { return nullptr; });
    auto refs     = DefVec(m.num_refs, [](size_t) { return nullptr; });
    if (!m.fn(root, swap, metas.data(), refs.data())) return;

    auto alt = Alt();
    alt.rule = &r;
    alt.cost = pbqp::Cost(r.cost);
    alt.covers.resize(m.num_covers, Cover{nullptr, 0, {}, false});
    for (size_t j = 0; j != m.num_refs; ++j) {
        auto info                  = rules::all_refs[m.refs + j];
        auto ex                    = refs[j]->isa<Extract>();
        auto cover                 = ex ? ex->tuple() : refs[j];
        alt.covers[info.cover].def = cover;
        if (info.kind == rules::Ref::Mem && uses(refs[j]) != 1) return;
    }

    // A pure inner node that something else uses is looked through: the match takes its operands but not the node.
    auto parent = std::vector<u8>(m.num_covers, 0);
    auto owner  = [&](u8 o) {
        while (o != 0 && alt.covers[o - 1].through)
            o = parent[o - 1];
        return o;
    };
    auto node    = [&](u8 o) { return o == 0 ? root : alt.covers[o - 1].def; };
    auto demands = [&](u8 o) -> auto& { return o == 0 ? alt.demands : alt.covers[o - 1].demands; };
    for (size_t j = 0; j != m.num_uses; ++j) {
        auto u = rules::all_uses[m.uses + j];
        auto o = owner(u.owner);
        if (u.ref) {
            auto def  = refs[u.index];
            auto c    = rules::all_refs[m.refs + u.index].cover;
            auto& cov = alt.covers[c];
            parent[c] = o;
            if (!root_of(cov.def)) return;
            if (sole(node(u.owner), def) != cov.def) {
                if (mem_valued(cov.def) || c + 1u == r.maps) return;
                cov.through = true;
                continue;
            }
            demands(o).emplace_back(Demand{def, Want::Reg, size_t(c) + 1});
        } else if (auto w = want_of(r, u.index); w == Want::Lit) {
            if (!Lit::isa(metas[u.index])) return;
        } else if (w != Want::Param) {
            auto def = metas[u.index];
            if (is_literal(w) && !literal(def, w)) return;
            demands(o).emplace_back(Demand{def, w});
        }
    }
    if (!holds(alt)) return;
    alt.metas = std::move(metas);
    alts.emplace_back(std::move(alt));
}

const Def* ISel::build(const Alt& alt) {
    const auto& r = *alt.rule;
    auto holes    = DefVec(r.num_metas, [&](size_t i) {
        auto w = want_of(r, i);
        return is_param(w) ? rewrite(alt.metas[i]) : hole(alt.metas[i], w);
    });
    auto res      = r.build(new_world(), holes.data());
    if (!r.maps) return res;
    map(alt.covers[r.maps - 1].def, res);
    return res->proj(2, 1);
}

void ISel::match(const Def* def, Alts& alts) {
    auto r = root_of(def);
    if (!r) return;
    for (const auto& rule : rules::rewrites)
        if (rule.root == *r) append(rule, def, alts);
}

const App* ISel::sole(const Def* parent, const Def* def) const {
    auto app = parent->isa<App>();
    if (!app || !root_of(app) || uses(app->arg()) != 1) return nullptr;
    if (std::ranges::count(DefVec(app->arg()->projs()), def) != 1) return nullptr;
    if (def->isa<Extract>() && key(def) != def) return sole_value(def);
    return uses(def) == 1 ? def->isa<App>() : nullptr;
}

bool ISel::holds(const Alt& alt) {
    // The replacement sits at the inner node whose memory it replaces, so the root's holes must be available there.
    if (!alt.rule->maps) return true;
    auto ld = alt.covers[alt.rule->maps - 1].def->as<App>();
    auto bb = chain(ld->arg(2, 0));
    if (!bb) return false;
    for (const auto& d : alt.demands) {
        auto done = DefSet();
        if (!d.inner && (!d.def->in_scope(bb) || depends(d.def, ld, done))) return false;
    }
    return true;
}

/*
 * Selection
 */

bool ISel::analyze() {
#ifdef MIM_ENABLE_CHECKS
    check_rules();
#endif
    for (auto mut : old_world().externals().muts())
        count(mut);

    auto defs = DefVec(counted_.begin(), counted_.end());
    std::ranges::sort(defs, [](const Def* a, const Def* b) { return a->gid() < b->gid(); });

    auto nodes = DefVec();
    for (auto def : defs) {
        auto alts = Alts();
        match(def, alts);
        if (alts.empty()) continue;
        alts_[def] = std::move(alts);
        nodes.emplace_back(def);
    }

    // Each inner node of a match becomes an alternative of the node it covers; equal ones are one class.
    // A cover's operands follow it in preorder, so their classes are known before its own.
    auto resolve = [](const Alt& alt, fe::Vector<Demand>& demands) {
        for (auto& d : demands)
            if (d.inner) d.inner = alt.covers[d.inner - 1].inner;
    };
    for (auto def : nodes)
        for (auto& alt : alts_[def]) {
            for (size_t j = alt.covers.size(); j-- != 0;) {
                auto& cover = alt.covers[j];
                if (cover.through) continue;
                resolve(alt, cover.demands);
                auto& inner = alts_.at(cover.def);
                auto same   = [&](const Alt& a) { return !a.rule && std::ranges::equal(a.demands, cover.demands); };
                auto i      = std::ranges::find_if(inner, same);
                cover.inner = size_t(i - inner.begin()) + 1;
                if (i == inner.end()) {
                    auto a    = Alt();
                    a.demands = cover.demands;
                    inner.emplace_back(std::move(a));
                }
            }
            resolve(alt, alt.demands);
        }

    auto graph = pbqp::Graph();
    auto ids   = DefMap<pbqp::NodeId>();
    for (auto def : nodes) {
        auto costs = pbqp::Costs();
        for (const auto& alt : alts_[def]) {
            auto c = alt.cost;
            for (const auto& d : alt.demands)
                if (!alts_.contains(key(d.def))) c += d.inner ? pbqp::Cost::inf() : leaf(d.def, d.want);
            costs.emplace_back(c);
        }
        ids[def] = graph.add(costs);
    }

    for (auto def : nodes) {
        const auto& alts = alts_[def];
        auto operands    = DefVec();
        for (const auto& alt : alts)
            for (const auto& d : alt.demands)
                if (auto op = key(d.def); alts_.contains(op) && std::ranges::find(operands, op) == operands.end())
                    operands.emplace_back(op);
        for (auto op : operands) {
            const auto& op_alts = alts_[op];
            auto costs          = pbqp::Matrix(alts.size(), op_alts.size(), [&](size_t i, size_t j) {
                const auto& o = op_alts[j];
                auto c        = pbqp::Cost(0);
                for (const auto& d : alts[i].demands) {
                    if (key(d.def) != op) continue;
                    if (d.inner)
                        c += !o.rule && j + 1 == d.inner ? 0 : pbqp::Cost::inf();
                    else
                        c += o.rule ? conv(o.rule->type, d.want) : pbqp::Cost::inf();
                }
                return c;
            });
            if (std::ranges::any_of(costs, [](pbqp::Cost c) { return c != 0; }))
                graph.add_edge_costs(ids[def], ids[op], costs);
        }
    }

    auto fallback = pbqp::Choices(graph.num_nodes(), 0);
    for (auto def : nodes) {
        auto i             = std::ranges::find_if(alts_[def], [](const Alt& a) { return a.rule && atomic(*a.rule); });
        auto found         = i != alts_[def].end();
        fallback[ids[def]] = found ? size_t(i - alts_[def].begin()) : 0;
#ifdef MIM_ENABLE_CHECKS
        if (!found) fe::throwf("x86 isel: `{}` has no atomic rule", def);
#endif
    }
#ifdef MIM_ENABLE_CHECKS
    if (pbqp::evaluate(graph, fallback).is_inf()) fe::throwf("the atomic rules must cover the program");
#endif
    auto sol = pbqp::solve(graph);
    log().d("x86 isel: {} nodes, {} edges, cost {}", graph.num_nodes(), graph.num_edges(), sol.cost.value());
    // RN is a heuristic, but the atomic rules always remain a valid cover.
    const auto& picked = sol.feasible() ? sol.choices : fallback;
    if (auto d = arg_value(args(), "isel-dot")) {
        auto out = Out(std::string(*d));
        auto& os = *out.os();
        for (auto def : nodes)
            std::println(os, "// n{} = {}", ids[def], def);
        pbqp::write_dot(os, graph, picked);
    }
    for (auto def : nodes) {
        auto c        = picked[ids[def]];
        choices_[def] = c;
        for (const auto& cover : alts_[def][c].covers)
            if (!cover.through) roots_[cover.def] = def;
    }
    return false;
}

#ifdef MIM_ENABLE_CHECKS
void ISel::check_rules() const {
    for (const auto& r : rules::rewrites) {
        if (conv(r.type, Want::Reg).is_inf())
            fe::throwf("x86 isel: rule `{}` yields a value that does not convert to a register", r.name);
        for (size_t i = 0; i != r.num_metas; ++i) {
            auto w = want_of(r, i);
            if (is_param(w) || is_literal(w)) continue; // no rule type fills it
            bool ok = false;
            for (size_t t = 0; t != Num_Tys; ++t)
                if (Ty(t) != Ty::Lit && !conv(Ty(t), w).is_inf()) ok = true;
            if (!ok) fe::throwf("x86 isel: no rule type converts to a hole of `{}`", r.name);
        }
    }

    for (const auto& r : rules::rewrites)
        if (!has_atomic(r.root)) fe::throwf("x86 isel: no atomic rule shares the root of `{}`", r.name);
}
#endif

void ISel::count(const Def* def) {
    if (!counted_.emplace(def).second) return;
    for (auto op : def->ops()) {
        if (op) {
            ++uses_[op];
            count(op);
        }
    }
}

const App* ISel::sole_value(const Def* def) const {
    if (auto app = key(def)->isa<App>(); app != def && uses(def) == 1 && uses(app) == 2)
        if (auto m = app->proj(2, 0); m && uses(m) != 0) return app;
    return nullptr;
}

const Def* ISel::key(const Def* def) const {
    def = strip(def);
    if (auto ex = def->isa<Extract>(); ex && Lit::isa(ex->index()) == 1)
        if (auto app = ex->tuple()->isa<App>(); app && root_of(app) && mem_valued(app)) return app;
    return def;
}

const Alt* ISel::choice(const Def* def) const {
    if (auto c = choices_.find(def); c != choices_.end()) return &alts_.at(def)[c->second];
    return nullptr;
}

/*
 * Replacement
 */

const Def* ISel::select(const Def* def) {
    if (auto i = repls_.find(def); i != repls_.end()) return i->second;
    auto alt = choice(def);
    auto res = build(*alt);
    if (!res) def->blame(MIM_X86_BE "rule `{}` cannot select `{}`", alt->rule->name, def).bail();
    return repls_[def] = res;
}

const Def* ISel::selected(const Def* def) {
    auto alt = choice(def);
    if (!alt) return nullptr;
    if (alt->rule) return convert(select(def), alt->rule->type, Want::Reg);
    // The root of the match covering this inner node builds it.
    rewrite(roots_.at(def));
    return lookup(def);
}

const Def* ISel::hole(const Def* def, Want want) {
    if (auto alt = choice(strip(def)); alt && alt->rule) return convert(select(strip(def)), alt->rule->type, want);
    return convert(val(def), Ty::Reg, want);
}

const Def* ISel::convert(const Def* x, Ty from, Want want) {
    if (is_literal(want)) return x;
    auto& w = new_world();
    if (from == Ty::Reg && Lit::isa(x)) from = Ty::Lit;
    auto to = target(from, want).first;
    for (auto t = from; t != to;) {
        auto c     = chains().first[size_t(t)][size_t(to)];
        auto holes = DefVec(x->projs(c->arity));
        x          = c->build(w, holes.data());
        t          = c->to;
    }
    return x;
}

const Def* ISel::val(const Def* def) {
    auto res = rewrite(def);
    if (auto l = Lit::isa(res); l && res->type()->isa<Nat>()) return new_world().lit_i64(*l);
    return res;
}

const Def* ISel::rewrite_imm_Lit(const Lit* lit) {
    if (lit->type()->isa<Nat>()) return new_world().lit_nat(lit->get());
    return RWPhase::rewrite_imm_Lit(lit);
}

const Def* ISel::coerce_leaf(const Def* type, const Def* val) {
    if (auto l = Lit::isa(val); l && val->type()->isa<Nat>() && type == new_world().type_i64())
        return new_world().lit_i64(*l);
    return nullptr;
}

const Def* ISel::rewrite_imm_Extract(const Extract* ex) {
    if (auto res = selected(ex)) return res;
    return RWPhase::rewrite_imm_Extract(ex);
}

const Def* ISel::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (Axm::isa<core::Ptr>(app)) return w.type_i64();
    if (Axm::isa<core::M>(app)) return w.call<core::M>(w.lit_nat_0());
    if (auto res = selected(app)) return res;
    if (auto ptr = strip(app); ptr != app) return rewrite(ptr);
    // Loop hints for LLVM.
    if (Axm::isa<ll::vec>(app) || Axm::isa<ll::novec>(app)) return rewrite(app->arg());

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !app->type()->isa<Type>()
                               && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
