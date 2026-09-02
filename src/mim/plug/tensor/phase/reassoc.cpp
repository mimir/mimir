#include "mim/plug/tensor/phase/reassoc.h"

#include <charconv>

#include <algorithm>
#include <array>

#include <fe/format.h>
#include <fe/worklist.h>

#include <mim/def.h>
#include <mim/plugin.h>
#include <mim/tuple.h>

#include <mim/util/types.h>

#include <mim/plug/core/core.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

/// Beyond this the eager enumeration of `Catalan(n − 1)` bracketings is worth a warning.
constexpr u64 Loud_max_dispatch = 8;

/// The symbolic extents of one `dims[i] · dims[s + 1] · dims[j + 1]` cost term, sorted by Def::gid and
/// padded with `nullptr`; literal extents fold into the coefficient instead.
using Mono = std::array<const Def*, 3>;

/// A chain cost as a polynomial in the symbolic extents.
class Poly {
public:
    void add(Mono mono, u64 coeff) {
        for (auto& [m, c] : terms_)
            if (m == mono) {
                c += coeff;
                return;
            }
        terms_.emplace_back(mono, coeff);
    }

    void add(const Poly& other) {
        for (const auto& [m, c] : other.terms_)
            add(m, c);
    }

    u64 coeff(Mono mono) const {
        for (const auto& [m, c] : terms_)
            if (m == mono) return c;
        return 0;
    }

    /// Is `this` at most @p other under *every* instantiation of the symbolic extents?
    /// Extents are non-negative, so a coefficient-wise `≤` is sufficient - but not necessary, which is
    /// what makes this order partial.
    bool dominates(const Poly& other) const {
        for (const auto& [m, c] : terms_)
            if (c > other.coeff(m)) return false;
        return true;
    }

    std::string str() const {
        if (terms_.empty()) return "0";
        auto s = std::string();
        for (const auto& [mono, coeff] : terms_) {
            if (!s.empty()) s += " + ";
            s += std::format("{}", coeff);
            for (auto d : mono)
                if (d) s += std::format("·{}", d);
        }
        return s;
    }

private:
    fe::Vector<std::pair<Mono, u64>> terms_;
};

/// The cost of one product `«x, y» · «y, z»`.
Poly mul_cost(const Def* x, const Def* y, const Def* z) {
    auto coeff = 1_u64;
    auto syms  = DefVec();

    for (auto d : {x, y, z})
        if (auto l = Lit::isa<u64>(d))
            coeff *= *l;
        else
            syms.emplace_back(d);
    std::ranges::sort(syms, [](const Def* a, const Def* b) { return a->gid() < b->gid(); });

    auto mono = Mono{};
    std::ranges::copy(syms, mono.begin());

    auto poly = Poly();
    poly.add(mono, coeff);
    return poly;
}

Poly cost_of(fe::View<Split> splits, Defs dims) {
    auto poly = Poly();
    for (auto [i, s, j] : splits)
        poly.add(mul_cost(dims[i], dims[s + 1], dims[j + 1]));
    return poly;
}

/// Every bracketing of `lo … hi`.
fe::Vector<Splits> bracketings(u64 lo, u64 hi) {
    if (lo == hi) return {Splits()};

    auto res = fe::Vector<Splits>();
    for (auto s = lo; s != hi; ++s)
        for (const auto& l : bracketings(lo, s))
            for (const auto& r : bracketings(s + 1, hi)) {
                auto b = l;
                b.append_range(r);
                b.emplace_back(Split{lo, s, hi});
                res.emplace_back(std::move(b));
            }
    return res;
}

/// Drops every bracketing that another one provably beats; equal costs keep the first.
/// A single survivor is hence the optimum under *every* instantiation of the symbolic extents.
/// Several survivors need not each win for some instantiation - domination is only sufficient for `≤`.
fe::Vector<Splits> pareto(fe::View<Splits> cands, Defs dims) {
    auto keep  = fe::Vector<Splits>();
    auto costs = fe::Vector<Poly>();

    for (const auto& cand : cands) {
        auto cost = cost_of(cand, dims);
        if (std::ranges::any_of(costs, [&](const Poly& k) { return k.dominates(cost); })) continue;
        for (auto i = costs.size(); i-- != 0;)
            if (cost.dominates(costs[i])) keep.erase(keep.begin() + i), costs.erase(costs.begin() + i);
        keep.emplace_back(cand);
        costs.emplace_back(std::move(cost));
    }

    return keep;
}

fe::Vector<u64> split_table(const Splits& splits, u64 n) {
    auto table = fe::Vector<u64>(n * n, 0);
    for (auto [i, s, j] : splits)
        table[i * n + j] = s;
    return table;
}

/// Matrix-chain order: matrix `i` of the chain has shape `dims[i] × dims[i + 1]`.
/// @returns the split table - `split[i * n + j]` is the last matrix of the left factor of the cheapest
/// parenthesization of `i … j` - together with that parenthesization's cost, or nothing at all if some
/// subchain has no candidate that provably beats all the others.
std::optional<std::pair<fe::Vector<u64>, Poly>> matrix_chain_order(Defs dims) {
    auto n     = dims.size() - 1;
    auto cost  = fe::Vector<Poly>(n * n);
    auto split = fe::Vector<u64>(n * n, 0);

    for (auto len = 2_u64; len <= n; ++len) {
        for (auto i = 0_u64; i + len <= n; ++i) {
            auto j     = i + len - 1;
            auto cands = fe::Vector<Poly>();
            for (auto s = i; s != j; ++s) {
                auto c = mul_cost(dims[i], dims[s + 1], dims[j + 1]);
                c.add(cost[i * n + s]);
                c.add(cost[(s + 1) * n + j]);
                cands.emplace_back(std::move(c));
            }

            auto best = std::ranges::find_if(cands, [&](const Poly& a) {
                return std::ranges::all_of(cands, [&](const Poly& b) { return a.dominates(b); });
            });
            if (best == cands.end()) return {};

            split[i * n + j] = i + (best - cands.begin());
            cost[i * n + j]  = std::move(*best);
        }
    }

    return std::pair{std::move(split), std::move(cost[n - 1])};
}

/// Charges @p d to the enclosing non-tuple consumer; tuples and packs are transparent arg wrappers.
void count_consumers(const Def* d, DefMap<u64>& consumers) {
    if (Axm::isa<tensor::product_2d>(d)) {
        ++consumers[d];
    } else if (d->isa<Tuple>() || d->isa<Pack>()) {
        for (auto op : d->ops())
            if (op) count_consumers(op, consumers);
    }
}

} // namespace

void Reassoc::start() {
    if (auto val = arg_value(args(), "reassoc-max")) {
        auto n   = 0_u64;
        auto end = val->data() + val->size();
        if (auto [ptr, ec] = std::from_chars(val->data(), end, n); ec != std::errc() || ptr != end) {
            log().w("ignoring `-X tensor:reassoc-max={}`: not a number", *val);
        } else {
            max_dispatch_ = n;
            log().d("dispatch chains of up to {} matrices", n);
            if (n > Loud_max_dispatch)
                log().w("`-X tensor:reassoc-max={}` enumerates up to Catalan({}) bracketings", n, n - 1);
        }
    }

    // The old world does not track uses, so count consumers up front: flatten() may only pull a
    // product apart where doing so cannot leave it materialized for another consumer as well.
    auto wl = fe::BFSWorklist<DefSet>();
    for (auto root : old_world().roots())
        wl.push(root);

    while (!wl.empty()) {
        auto def = wl.pop();
        if (!def->isa<Tuple>() && !def->isa<Pack>())
            for (auto op : def->ops())
                if (op) count_consumers(op, consumers_);
        for (auto op : def->ops())
            if (op) wl.push(op);
        if (def->type()) wl.push(def->type());
    }

    RWPhase::start();
}

std::optional<Reassoc::Link> Reassoc::isa_link(const Def* def, const Def* ring) const {
    auto app = Axm::isa<tensor::product_2d>(def);
    if (!app) return {};

    // The curry chain, outermost app first: [t1, t2] {m k l} [R].
    auto groups = app->callee()->as<App>();
    if (groups->callee()->as<App>()->arg() != ring) return {};

    auto [m, k, l] = groups->args<3>();
    return Link{app, m, k, l};
}

void Reassoc::flatten(const Def* def, const Def* ring, const Def* rows, DefVec& mats, DefVec& dims, Splits& orig) {
    auto lo = mats.size();

    if (auto i = consumers_.find(def); i != consumers_.end() && i->second == 1)
        if (auto link = isa_link(def, ring)) {
            auto [t1, t2] = link->app->args<2>();
            flatten(t1, ring, link->m, mats, dims, orig);
            auto mid = mats.size();
            flatten(t2, ring, link->k, mats, dims, orig);
            orig.emplace_back(Split{lo, mid - 1, mats.size() - 1});
            return;
        }

    mats.emplace_back(def);
    dims.emplace_back(rows);
}

const Def* Reassoc::build(const Def* head, Defs mats, Defs dims, fe::View<u64> split, u64 i, u64 j) {
    if (i == j) return rewrite(mats[i]);

    auto& w  = new_world();
    auto s   = split[i * mats.size() + j];
    auto t1  = build(head, mats, dims, split, i, s);
    auto t2  = build(head, mats, dims, split, s + 1, j);
    auto mkl = DefVec{rewrite(dims[i]), rewrite(dims[s + 1]), rewrite(dims[j + 1])};
    return w.app(w.app(head, mkl), {t1, t2});
}

const Def* Reassoc::cost_expr(Defs dims, const Splits& splits) {
    auto& w        = new_world();
    const Def* sum = nullptr;

    for (auto [i, s, j] : splits) {
        auto p = w.app(w.annex(core::nat::mul), {rewrite(dims[i]), rewrite(dims[s + 1])});
        p      = w.app(w.annex(core::nat::mul), {p, rewrite(dims[j + 1])});
        sum    = sum ? w.app(w.annex(core::nat::add), {sum, p}) : p;
    }

    return sum;
}

const Def* Reassoc::dispatch(const Def* head, const Def* res_ty, Defs mats, Defs dims, fe::View<Splits> cands) {
    auto& w = new_world();
    auto n  = mats.size();
    auto pi = w.pi(w.sigma(), res_ty);

    // Each bracketing goes behind a thunk so that only the selected one runs. The filter is `tt`, so once
    // the comparison folds - a caller that knows the extents, `%compile.lam_spec` - the winner inlines and
    // the losers become unreachable.
    const Def* best      = nullptr;
    const Def* best_cost = nullptr;
    for (const auto& cand : cands) {
        auto thunk = w.mut_lam(pi)->set(true, build(head, mats, dims, split_table(cand, n), 0, n - 1));
        auto cost  = cost_expr(dims, cand);

        if (!best) {
            best = thunk, best_cost = cost;
        } else {
            auto cheaper = w.app(w.annex(core::ncmp::l), {cost, best_cost});
            best         = w.extract(w.tuple({best, (const Def*)thunk}), cheaper);
            best_cost    = w.extract(w.tuple({best_cost, cost}), cheaper);
        }
    }

    return w.app(best, w.tuple());
}

const Def* Reassoc::reassoc(const App* app) {
    auto head = app->callee()->as<App>()->callee()->as<App>();
    auto link = isa_link(app, head->arg());
    if (!link) return nullptr;

    auto [t1, t2] = app->args<2>();
    auto mats     = DefVec();
    auto dims     = DefVec();
    auto orig     = Splits();
    flatten(t1, head->arg(), link->m, mats, dims, orig);
    auto mid = mats.size();
    flatten(t2, head->arg(), link->k, mats, dims, orig);
    dims.emplace_back(link->l);
    orig.emplace_back(Split{0_u64, mid - 1, mats.size() - 1});

    // Two matrices admit only one parenthesization.
    auto n = mats.size();
    if (n < 3) return nullptr;

    auto orig_cost = cost_of(orig, dims);

    if (n <= max_dispatch_) {
        auto cands = pareto(bracketings(0, n - 1), dims);
        if (cands.size() != 1) {
            log().d("dispatch chain {} over {} bracketings, written as {}", fe::Join(dims, "×"), cands.size(),
                    orig_cost.str());
            return dispatch(rewrite(head), rewrite(app->type()), mats, dims, cands);
        }

        auto cost = cost_of(cands.front(), dims);
        if (orig_cost.dominates(cost)) return nullptr;
        log().d("reassociate chain {}: {} → {} multiplications", fe::Join(dims, "×"), orig_cost.str(), cost.str());
        return build(rewrite(head), mats, dims, split_table(cands.front(), n), 0, n - 1);
    }

    auto order = matrix_chain_order(dims);
    if (!order) return nullptr;

    auto& [split, cost] = *order;
    if (orig_cost.dominates(cost)) return nullptr;

    log().d("reassociate chain {}: {} → {} multiplications", fe::Join(dims, "×"), orig_cost.str(), cost.str());
    return build(rewrite(head), mats, dims, split, 0, n - 1);
}

const Def* Reassoc::rewrite_imm_App(const App* app) {
    if (Axm::isa<tensor::product_2d>(app))
        if (auto res = reassoc(app)) return res;
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
