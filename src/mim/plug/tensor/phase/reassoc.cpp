#include "mim/plug/tensor/phase/reassoc.h"

#include <algorithm>

#include <fe/format.h>
#include <fe/worklist.h>

#include <mim/def.h>
#include <mim/tuple.h>

#include <mim/util/types.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

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

Poly cost_of(fe::View<std::array<u64, 3>> splits, Defs dims) {
    auto poly = Poly();
    for (auto [i, s, j] : splits)
        poly.add(mul_cost(dims[i], dims[s + 1], dims[j + 1]));
    return poly;
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
            orig.emplace_back(std::array{lo, mid - 1, mats.size() - 1});
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

const Def* Reassoc::reassoc(const App* app) {
    auto head = app->callee()->as<App>()->callee()->as<App>();
    auto link = isa_link(app, head->arg());
    if (!link) return nullptr;

    auto [t1, t2] = app->args<2>();
    auto mats     = DefVec();
    auto dims     = DefVec();
    auto splits   = Splits();
    flatten(t1, head->arg(), link->m, mats, dims, splits);
    auto mid = mats.size();
    flatten(t2, head->arg(), link->k, mats, dims, splits);
    dims.emplace_back(link->l);
    splits.emplace_back(std::array{0_u64, mid - 1, mats.size() - 1});

    // Two matrices admit only one parenthesization.
    if (mats.size() < 3) return nullptr;

    auto order = matrix_chain_order(dims);
    if (!order) return nullptr;

    auto& [split, cost] = *order;
    auto orig           = cost_of(splits, dims);
    if (orig.dominates(cost)) return nullptr;

    log().d("reassociate chain {}: {} → {} multiplications", fe::Join(dims, "×"), orig.str(), cost.str());
    return build(rewrite(head), mats, dims, split, 0, mats.size() - 1);
}

const Def* Reassoc::rewrite_imm_App(const App* app) {
    if (Axm::isa<tensor::product_2d>(app))
        if (auto res = reassoc(app)) return res;
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
