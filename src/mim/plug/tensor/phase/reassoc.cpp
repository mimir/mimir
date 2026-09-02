#include "mim/plug/tensor/phase/reassoc.h"

#include <fe/format.h>
#include <fe/worklist.h>

#include <mim/def.h>
#include <mim/tuple.h>

#include <mim/util/types.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

using Row = fe::Vector<u64>;

/// Matrix-chain order: matrix `i` of the chain has shape `dims[i] × dims[i + 1]`.
/// @returns the split table - `split[i * n + j]` is the last matrix of the left factor of the cheapest
/// parenthesization of `i … j` - together with that parenthesization's cost.
std::pair<Row, u64> matrix_chain_order(fe::View<u64> dims) {
    auto n     = dims.size() - 1;
    auto cost  = Row(n * n, 0);
    auto split = Row(n * n, 0);

    for (auto len = 2_u64; len <= n; ++len) {
        for (auto i = 0_u64; i + len <= n; ++i) {
            auto j          = i + len - 1;
            cost[i * n + j] = u64(-1);
            for (auto s = i; s != j; ++s) {
                auto c = cost[i * n + s] + cost[(s + 1) * n + j] + dims[i] * dims[s + 1] * dims[j + 1];
                if (c < cost[i * n + j]) cost[i * n + j] = c, split[i * n + j] = s;
            }
        }
    }

    return {std::move(split), cost[n - 1]};
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

    auto [m, k, l] = groups->args<3>([](const Def* d) { return Lit::isa<u64>(d); });
    if (!m || !k || !l) return {};
    return Link{app, *m, *k, *l};
}

void Reassoc::flatten(const Def* def, const Def* ring, u64 rows, DefVec& mats, Row& dims, u64& cost) {
    if (auto i = consumers_.find(def); i != consumers_.end() && i->second == 1)
        if (auto link = isa_link(def, ring)) {
            auto [t1, t2] = link->app->args<2>();
            flatten(t1, ring, link->m, mats, dims, cost);
            flatten(t2, ring, link->k, mats, dims, cost);
            cost += link->m * link->k * link->l;
            return;
        }

    mats.emplace_back(def);
    dims.emplace_back(rows);
}

const Def* Reassoc::build(const Def* head, Defs mats, fe::View<u64> dims, fe::View<u64> split, u64 i, u64 j) {
    if (i == j) return rewrite(mats[i]);

    auto& w  = new_world();
    auto s   = split[i * mats.size() + j];
    auto t1  = build(head, mats, dims, split, i, s);
    auto t2  = build(head, mats, dims, split, s + 1, j);
    auto mkl = DefVec{w.lit_nat(dims[i]), w.lit_nat(dims[s + 1]), w.lit_nat(dims[j + 1])};
    return w.app(w.app(head, mkl), {t1, t2});
}

const Def* Reassoc::reassoc(const App* app) {
    auto head = app->callee()->as<App>()->callee()->as<App>();
    auto link = isa_link(app, head->arg());
    if (!link) return nullptr;

    auto [t1, t2] = app->args<2>();
    auto mats     = DefVec();
    auto dims     = Row();
    auto orig     = link->m * link->k * link->l;
    flatten(t1, head->arg(), link->m, mats, dims, orig);
    flatten(t2, head->arg(), link->k, mats, dims, orig);
    dims.emplace_back(link->l);

    // Two matrices admit only one parenthesization.
    if (mats.size() < 3) return nullptr;

    auto [split, cost] = matrix_chain_order(dims);
    if (cost >= orig) return nullptr;

    log().d("reassociate chain {}: {} → {} multiplications", fe::Join(dims, "×"), orig, cost);
    return build(rewrite(head), mats, dims, split, 0, mats.size() - 1);
}

const Def* Reassoc::rewrite_imm_App(const App* app) {
    if (Axm::isa<tensor::product_2d>(app))
        if (auto res = reassoc(app)) return res;
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
