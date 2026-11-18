#include "pbqp/solver.h"

#include <cassert>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <numeric>
#include <optional>
#include <queue>
#include <thread>

#include <fe/arena.h>
#include <fe/vector.h>

#if defined(__SANITIZE_THREAD__)
#    define PBQP_TSAN 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define PBQP_TSAN 1
#    endif
#endif

// ifunc resolvers run during relocation, before the ThreadSanitizer runtime exists.
#if defined(__x86_64__) && defined(__ELF__) && (defined(__GNUC__) || defined(__clang__)) && !defined(PBQP_TSAN)
#    define PBQP_DISPATCH __attribute__((target_clones("arch=x86-64-v4", "avx2", "default")))
#else
#    define PBQP_DISPATCH
#endif

namespace pbqp {
namespace {

/// By value, as GCC fails to vectorize `std::min` over references to a class.
Cost min(Cost lhs, Cost rhs) { return lhs.value() < rhs.value() ? lhs : rhs; }

/// Only valid for normalized, hence non-negative, costs: their sum fits into 32 unsigned bits.
Cost add_normalized(Cost lhs, Cost rhs) {
    const auto sum = static_cast<std::uint32_t>(lhs.value()) + static_cast<std::uint32_t>(rhs.value());
    return static_cast<Cost::Rep>(std::min(sum, static_cast<std::uint32_t>(Cost::inf().value())));
}

/// Only valid for normalized costs with `minimum <= value`; an inf minimum means the whole row was moved elsewhere.
Cost sub_normalized(Cost value, Cost minimum) {
    if (minimum.is_inf()) return 0;
    if (value.is_inf()) return Cost::inf();
    return static_cast<Cost::Rep>(std::min<int64_t>(int64_t{value.value()} - minimum.value(), Cost::inf().value()));
}

PBQP_DISPATCH void add_into(Cost* __restrict dst, const Cost* __restrict src, size_t count) {
    for (size_t i = 0; i < count; ++i)
        dst[i] = add_normalized(dst[i], src[i]);
}

PBQP_DISPATCH void fold_row(Cost* __restrict dst, const Cost* __restrict row, Cost cost, size_t count) {
    for (size_t i = 0; i < count; ++i)
        dst[i] = min(dst[i], add_normalized(cost, row[i]));
}

/// Matrix of an edge oriented (reduced option, neighbour option).
struct TrimmedEdge {
    NodeId neighbour = 0;
    MatrixView matrix;
};

struct Reduction {
    fe::View<Cost> costs;
    fe::View<TrimmedEdge> edges;
};

/// What back-substitution needs of an edge; dead edges are never written, so their matrices stay as reduced.
struct Link {
    NodeId neighbour = 0;
    MatrixView matrix;
    const OptionId* neighbour_working = nullptr; // original -> working option or npos; null if none was deleted
    bool is_src                       = false;
};

/// A reduced node in working option order.
struct Subst {
    NodeId node              = 0;
    size_t num_options       = 0;
    size_t num_links         = 0;
    const Cost* costs        = nullptr;
    const OptionId* original = nullptr; // original[working option]; null if no option was deleted
    const Link* links        = nullptr;
};

/// Normalized working copy of a Graph with worklists for exact and RN reductions.
class Reducer {
public:
    Reducer(const Graph& graph, fe::View<size_t> rn_priority)
        : rn_priority_(rn_priority) {
        nodes_.resize(graph.num_nodes());
        for (const auto n : graph.nodes()) {
            const auto costs = graph.node_costs(n);
            nodes_[n].costs.assign(costs.begin(), costs.end());
            nodes_[n].original.resize(costs.size());
            nodes_[n].original_options = costs.size();
            std::ranges::iota(nodes_[n].original, OptionId{0});
            nodes_[n].alive = true;
        }
        assert((rn_priority.empty() || rn_priority.size() >= nodes_.size())
               && "solve: rn_priority does not cover every node");
        for (const auto e : graph.edges()) {
            const auto [src, dst] = graph.edge(e);
            link(src, dst, graph.edge_costs(e).data(), nullptr);
        }
        for (EdgeId e = 0; e < edges_.size(); ++e)
            normalize_edge(e);
        for (const auto n : graph.nodes()) {
            normalize_node(n);
            touch(n);
        }
    }

    [[nodiscard]] size_t slots() const { return nodes_.size(); }
    [[nodiscard]] const Statistics& statistics() const { return statistics_; }

    /// Exact reductions first; RN takes the highest priority, then degree, then rn_score.
    NodeId next() {
        while (!dirty_.empty()) {
            const NodeId n = dirty_.back();
            dirty_.pop_back();
            nodes_[n].dirty = false;
            if (nodes_[n].alive) prune(n);
        }
        while (!low_.empty()) {
            const NodeId n = low_.back();
            low_.pop_back();
            if (nodes_[n].alive) return n;
        }
        while (!high_.empty()) {
            const Candidate candidate = high_.top();
            high_.pop();
            const Node& node = nodes_[candidate.node];
            if (node.alive && node.version == candidate.version) return candidate.node;
        }
        return Graph::npos;
    }

    void reduce(NodeId n, fe::Vector<Subst>& stack) {
        if (degree(n) > 2 && options(n) > 1) {
            bool merged = false;
            while (merge_neighbour(n, stack))
                merged = true;
            if (merged) {
                touch(n);
                return;
            }
        }

        stack.emplace_back(record(n));
        const auto mark           = scratch_.state();
        const Reduction reduction = snapshot(n, stack.back());
        switch (reduction.edges.size()) {
            case 0: ++statistics_.r0; break;
            case 1:
                ++statistics_.r1;
                fold_into_neighbour(reduction);
                break;
            case 2:
                ++statistics_.r2;
                contract(reduction);
                break;
            default:
                ++(options(n) == 1 ? statistics_.single_option : statistics_.rn);
                fold_locally_best_option(reduction);
                break;
        }
        scratch_.deallocate(mark);
        remove_node(n);
    }

    /// Exact for degree <= 2 nodes; RN nodes may still improve on their RN choice here.
    [[nodiscard]] OptionId choose(const Subst& subst, const Choices& choices) const {
        const auto original = [&](OptionId x) { return subst.original ? subst.original[x] : x; };
        const size_t own    = subst.num_options;

        OptionId best   = original(0);
        Cost best_total = Cost::inf();
        for (OptionId x = 0; x < own; ++x) {
            Cost total = subst.costs[x];
            for (size_t l = 0; l < subst.num_links && !total.is_inf(); ++l) {
                const Link& link      = subst.links[l];
                const OptionId chosen = choices[link.neighbour];
                const OptionId y      = link.neighbour_working ? link.neighbour_working[chosen] : chosen;
                assert(y != Graph::npos && "choose: a neighbour picked an option it had already lost");
                total += link.is_src ? link.matrix[x, y] : link.matrix[y, x];
            }
            if (total < best_total) {
                best_total = total;
                best       = original(x);
            }
        }
        return best;
    }

private:
    struct Node {
        Costs costs;
        fe::Vector<OptionId, 4> original;  // original[working option]
        const OptionId* working = nullptr; // inverse of original once an option was deleted, built on demand
        Edges edges;
        size_t original_options = 0;
        size_t version          = 0;
        bool alive              = false;
        bool low                = false;
        bool dirty              = false;
    };

    struct Edge {
        NodeId src         = 0;
        NodeId dst         = 0;
        const Cost* shared = nullptr; // the input graph's matrix until the first write
        Cost* owned        = nullptr;
    };

    struct Candidate {
        size_t priority = 0;
        size_t degree   = 0;
        double score    = 0.0;
        NodeId node     = 0;
        size_t version  = 0;

        auto operator<=>(const Candidate&) const = default;
    };

    [[nodiscard]] size_t degree(NodeId n) const { return nodes_[n].edges.size(); }
    [[nodiscard]] size_t options(NodeId n) const { return nodes_[n].costs.size(); }

    [[nodiscard]] MatrixView data(EdgeId e) const {
        const Edge& edge = edges_[e];
        return {edge.owned != nullptr ? edge.owned : edge.shared, options(edge.src), options(edge.dst)};
    }

    [[nodiscard]] Matrix writable(EdgeId e) {
        if (edges_[e].owned == nullptr) edges_[e].owned = arena_.copy(data(e)).data();
        const Edge& edge = edges_[e];
        return {edge.owned, options(edge.src), options(edge.dst)};
    }

    /// Cost spread per incident edge: what a wrong RN decision is likely to cost.
    [[nodiscard]] double rn_score(NodeId n) const {
        const auto [low, high] = std::ranges::minmax(nodes_[n].costs);
        return static_cast<double>(high.value() - low.value()) / static_cast<double>(std::max<size_t>(1, degree(n)));
    }

    /// Single-option nodes are reduced exactly at any degree, so they belong on the low worklist.
    void touch(NodeId n) {
        Node& node = nodes_[n];
        ++node.version;
        if (!node.dirty) dirty_.emplace_back(n);
        node.dirty = true;
        if (degree(n) <= 2 || options(n) == 1) {
            if (!node.low) low_.emplace_back(n);
            node.low = true;
        } else {
            high_.push(Candidate{.priority = rn_priority_.empty() ? 0 : rn_priority_[n],
                                 .degree   = degree(n),
                                 .score    = rn_score(n),
                                 .node     = n,
                                 .version  = node.version});
        }
    }

    void normalize_node(NodeId n) {
        Costs& costs       = nodes_[n].costs;
        const Cost minimum = std::accumulate(costs.begin(), costs.end(), Cost::inf(), min);
        if (minimum == 0 || minimum.is_inf()) return;
        for (auto& cost : costs)
            cost = sub_normalized(cost, minimum);
    }

    /// Moves row and column minima into the node vectors and drops the edge if nothing is left.
    void normalize_edge(EdgeId e) {
        const NodeId src  = edges_[e].src;
        const NodeId dst  = edges_[e].dst;
        Costs& src_costs  = nodes_[src].costs;
        Costs& dst_costs  = nodes_[dst].costs;
        const size_t rows = src_costs.size();
        const size_t cols = dst_costs.size();

        for (size_t r = 0; r < rows; ++r) {
            const Cost* const row = data(e).row(r);
            const Cost minimum    = std::accumulate(row, row + cols, Cost::inf(), min);
            if (minimum == 0) continue;
            src_costs[r] += minimum;
            Matrix matrix = writable(e);
            for (size_t c = 0; c < cols; ++c)
                matrix[r, c] = sub_normalized(matrix[r, c], minimum);
        }
        for (size_t c = 0; c < cols; ++c) {
            const MatrixView matrix = data(e);
            Cost minimum            = Cost::inf();
            for (size_t r = 0; r < rows; ++r)
                minimum = min(minimum, matrix[r, c]);
            if (minimum == 0) continue;
            dst_costs[c] += minimum;
            Matrix writes = writable(e);
            for (size_t r = 0; r < rows; ++r)
                writes[r, c] = sub_normalized(writes[r, c], minimum);
        }
        if (std::ranges::all_of(data(e), [](Cost cost) { return cost == 0; })) {
            erase(nodes_[src].edges, e);
            erase(nodes_[dst].edges, e);
            ++statistics_.dropped_edges;
        }
    }

    [[nodiscard]] NodeId other_end(EdgeId e, NodeId n) const {
        return edges_[e].src == n ? edges_[e].dst : edges_[e].src;
    }

    void erase_option(NodeId n, OptionId a) {
        Node& node = nodes_[n];
        for (const auto e : node.edges) {
            Matrix matrix = writable(e);
            if (edges_[e].src == n)
                matrix.erase_row(a);
            else
                matrix.erase_col(a);
        }
        node.costs.erase(node.costs.begin() + static_cast<std::ptrdiff_t>(a));
        node.original.erase(node.original.begin() + static_cast<std::ptrdiff_t>(a));
        node.working = nullptr;
    }

    /// Deletes forbidden options, always keeping at least one.
    void prune(NodeId n) {
        bool changed = false;
        for (OptionId a = 0; a < options(n) && options(n) > 1;) {
            if (nodes_[n].costs[a].is_inf()) {
                erase_option(n, a);
                ++statistics_.forbidden_options;
                changed = true;
            } else {
                ++a;
            }
        }
        if (!changed) return;

        const Edges incident = nodes_[n].edges;
        for (const auto e : incident)
            normalize_edge(e);
        normalize_node(n);
        touch(n);
        for (const auto e : incident) {
            normalize_node(other_end(e, n));
            touch(other_end(e, n));
        }
    }

    [[nodiscard]] const OptionId* working_options(NodeId n) {
        Node& node = nodes_[n];
        if (node.original.size() == node.original_options || node.working != nullptr) return node.working;
        OptionId* const working = arena_.allocate<OptionId>(node.original_options);
        std::fill_n(working, node.original_options, Graph::npos);
        for (OptionId x = 0; x < node.original.size(); ++x)
            working[node.original[x]] = x;
        return node.working = working;
    }

    [[nodiscard]] Subst record(NodeId n) {
        const Node& node  = nodes_[n];
        Link* const links = arena_.allocate<Link>(degree(n));
        size_t l          = 0;
        for (const auto e : node.edges) {
            const NodeId other = other_end(e, n);
            new (links + l++) Link{.neighbour         = other,
                                   .matrix            = data(e),
                                   .neighbour_working = working_options(other),
                                   .is_src            = edges_[e].src == n};
        }
        const bool deleted = node.original.size() != node.original_options;
        return Subst{.node        = n,
                     .num_options = options(n),
                     .num_links   = degree(n),
                     .costs       = arena_.copy(node.costs).data(),
                     .original    = deleted ? arena_.copy(node.original).data() : nullptr,
                     .links       = links};
    }

    void link(NodeId m, NodeId n, const Cost* shared, Cost* owned) {
        edges_.emplace_back(Edge{.src = m, .dst = n, .shared = shared, .owned = owned});
        nodes_[m].edges.emplace_back(edges_.size() - 1);
        nodes_[n].edges.emplace_back(edges_.size() - 1);
    }

    [[nodiscard]] EdgeId find_edge(NodeId m, NodeId n) const {
        if (degree(n) < degree(m)) std::swap(m, n);
        for (const auto e : nodes_[m].edges)
            if (edges_[e].src == n || edges_[e].dst == n) return e;
        return Graph::npos;
    }

    void remove_node(NodeId n) {
        Node& node           = nodes_[n];
        const Edges incident = std::move(node.edges);
        node.edges.clear();
        node.alive = false;
        for (const auto e : incident) {
            const NodeId other = other_end(e, n);
            erase(nodes_[other].edges, e);
            touch(other);
        }
    }

    /// Views edges stored as (from, other) and transposes the others into the scratch arena.
    [[nodiscard]] TrimmedEdge trim_edge(EdgeId e, NodeId from) {
        const NodeId other      = other_end(e, from);
        const size_t rows       = options(from);
        const size_t cols       = options(other);
        const MatrixView matrix = data(e);
        if (edges_[e].src == from) return {.neighbour = other, .matrix = matrix};

        Matrix transposed(scratch_.allocate<Cost>(rows * cols), rows, cols);
        transposed.transpose_from(matrix);
        return {.neighbour = other, .matrix = transposed};
    }

    [[nodiscard]] Reduction snapshot(NodeId n, const Subst& subst) {
        TrimmedEdge* const edges = scratch_.allocate<TrimmedEdge>(degree(n));
        size_t i                 = 0;
        for (const auto e : nodes_[n].edges)
            new (edges + i++) TrimmedEdge(trim_edge(e, n));
        return {.costs = {subst.costs, subst.num_options}, .edges = {edges, degree(n)}};
    }

    /// RI: c'_n(y) = c_n(y) + min_x ( c(x) + M(x, y) )
    void fold_into_neighbour(const Reduction& reduction) {
        const TrimmedEdge& edge = reduction.edges.front();
        const size_t y_dim      = edge.matrix.cols();

        Cost* const best = scratch_.allocate<Cost>(y_dim);
        std::fill_n(best, y_dim, Cost::inf());
        for (size_t x = 0; x < reduction.costs.size(); ++x)
            fold_row(best, edge.matrix.row(x), reduction.costs[x], y_dim);
        add_into(nodes_[edge.neighbour].costs.data(), best, y_dim);
        normalize_node(edge.neighbour);
    }

    /// Whether the edge between `m` and `n` exists and is stored as (n, m).
    [[nodiscard]] bool reversed(NodeId m, NodeId n) const {
        const EdgeId e = find_edge(m, n);
        return e != Graph::npos && edges_[e].src != m;
    }

    /// RII: M'_mn(i, j) = min_x ( c(x) + M_m(x, i) + M_n(x, j) )
    void contract(const Reduction& reduction) {
        const TrimmedEdge* to_m = &reduction.edges[0];
        const TrimmedEdge* to_n = &reduction.edges[1];
        if (reversed(to_m->neighbour, to_n->neighbour)) std::swap(to_m, to_n);
        const size_t m_dim = to_m->matrix.cols();
        const size_t n_dim = to_n->matrix.cols();

        const auto state = arena_.state();
        Matrix merged(arena_.allocate<Cost>(m_dim * n_dim), m_dim, n_dim);
        std::ranges::fill(merged, Cost::inf());
        for (size_t x = 0; x < reduction.costs.size(); ++x) {
            const Cost* const n_row = to_n->matrix.row(x);
            for (size_t i = 0; i < m_dim; ++i)
                fold_row(merged.row(i), n_row, add_normalized(reduction.costs[x], to_m->matrix[x, i]), n_dim);
        }
        add_to_edge(to_m->neighbour, to_n->neighbour, merged, state);
    }

    /// Adds `matrix`, the newest allocation in the arena since `state`, to the edge between m and n.
    /// It is oriented like that edge, or (m, n) if there is none, and is kept whenever the edge needs a fresh matrix.
    void add_to_edge(NodeId m, NodeId n, Matrix matrix, fe::Arena::State state) {
        assert((matrix.rows() == options(m) && matrix.cols() == options(n)) && "add_to_edge: matrix orientation");
        EdgeId e = find_edge(m, n);
        assert((e == Graph::npos || edges_[e].src == m) && "add_to_edge: edge orientation");
        if (e == Graph::npos) {
            e = edges_.size();
            link(m, n, nullptr, matrix.data());
        } else if (edges_[e].owned == nullptr) {
            add_into(matrix.data(), edges_[e].shared, matrix.size());
            edges_[e].owned = matrix.data();
        } else {
            add_into(edges_[e].owned, matrix.data(), matrix.size());
            arena_.deallocate(state);
        }
        normalize_edge(e);
        normalize_node(m);
        normalize_node(n);
    }

    /// The option of `m` that each option of `n` forces, or null if some option of `n` leaves `m` a choice.
    [[nodiscard]] const OptionId* implied_options(EdgeId e, NodeId n) {
        const TrimmedEdge edge  = trim_edge(e, n);
        const Costs& m_costs    = nodes_[edge.neighbour].costs;
        OptionId* const implied = scratch_.allocate<OptionId>(options(n));
        std::fill_n(implied, options(n), Graph::npos);
        for (OptionId i = 0; i < options(n); ++i) {
            if (nodes_[n].costs[i].is_inf()) continue;
            for (OptionId j = 0; j < edge.matrix.cols(); ++j) {
                if (add_normalized(edge.matrix[i, j], m_costs[j]).is_inf()) continue;
                if (implied[i] != Graph::npos) return nullptr;
                implied[i] = j;
            }
        }
        return implied;
    }

    /// RM (Buchwald et al. 2011): moves the other edges of a neighbour whose option `n` determines onto `n`.
    bool merge_neighbour(NodeId n, fe::Vector<Subst>& stack) {
        // add_to_edge may grow nodes_[n].edges and dangle this loop's iterator, so a merge must return immediately.
        for (const auto e : nodes_[n].edges) {
            const NodeId m = other_end(e, n);
            if (degree(m) < 2) continue;
            const auto mark               = scratch_.state();
            const OptionId* const implied = implied_options(e, n);
            if (implied == nullptr) {
                scratch_.deallocate(mark);
                continue;
            }

            const Edges moved = nodes_[m].edges;
            for (const auto f : moved) {
                if (f == e) continue;
                const NodeId w         = other_end(f, m);
                const TrimmedEdge to_w = trim_edge(f, m);
                const size_t n_options = options(n);
                const size_t w_options = to_w.matrix.cols();
                const bool flip        = reversed(n, w);

                const auto state = arena_.state();
                Matrix delta(arena_.allocate<Cost>(n_options * w_options), flip ? w_options : n_options,
                             flip ? n_options : w_options);
                std::ranges::fill(delta, Cost{0});
                for (OptionId i = 0; i < n_options; ++i) {
                    if (implied[i] == Graph::npos) continue;
                    for (OptionId j = 0; j < w_options; ++j)
                        if (flip)
                            delta[j, i] = to_w.matrix[implied[i], j];
                        else
                            delta[i, j] = to_w.matrix[implied[i], j];
                }
                erase(nodes_[m].edges, f);
                erase(nodes_[w].edges, f);
                if (flip)
                    add_to_edge(w, n, delta, state);
                else
                    add_to_edge(n, w, delta, state);
                touch(w);
            }
            scratch_.deallocate(mark);
            ++statistics_.rm;
            reduce(m, stack);
            return true;
        }
        return false;
    }

    /// RN: commit to the locally best option and charge its matrix rows to the neighbours.
    void fold_locally_best_option(const Reduction& reduction) {
        OptionId best_option = 0;
        Cost best_total      = Cost::inf();
        for (OptionId x = 0; x < reduction.costs.size(); ++x) {
            Cost total = reduction.costs[x];
            for (const auto& edge : reduction.edges) {
                const Costs& neighbour_costs = nodes_[edge.neighbour].costs;
                Cost cheapest                = Cost::inf();
                for (size_t y = 0; y < edge.matrix.cols(); ++y)
                    cheapest = min(cheapest, neighbour_costs[y] + edge.matrix[x, y]);
                total += cheapest;
            }
            if (total < best_total) {
                best_total  = total;
                best_option = x;
            }
        }
        for (const auto& edge : reduction.edges) {
            add_into(nodes_[edge.neighbour].costs.data(), edge.matrix.row(best_option), edge.matrix.cols());
            normalize_node(edge.neighbour);
        }
    }

    fe::Arena arena_;
    fe::Arena scratch_;
    fe::Vector<Node> nodes_;
    fe::Vector<Edge> edges_;
    fe::Vector<NodeId> low_;
    fe::Vector<NodeId> dirty_;
    fe::View<size_t> rn_priority_;
    Statistics statistics_;
    std::priority_queue<Candidate, fe::Vector<Candidate>> high_;
};

} // namespace

Cost evaluate(const Graph& graph, fe::View<OptionId> choices) {
    Cost total = 0;

    for (const auto n : graph.nodes()) {
        assert(n < choices.size() && "evaluate: choices is too short");
        const auto costs = graph.node_costs(n);
        assert(choices[n] < costs.size() && "evaluate: option index out of range");
        total += costs[choices[n]];
    }
    for (const auto e : graph.edges()) {
        const auto [src, dst] = graph.edge(e);
        total += graph.edge_matrix(e)[choices[src], choices[dst]];
    }
    return total;
}

fe::Bitset conflicts(const Graph& graph, fe::View<OptionId> choices) {
    fe::Bitset result;
    for (const auto n : graph.nodes()) {
        assert(n < choices.size() && "conflicts: choices is too short");
        if (graph.node_costs(n)[choices[n]].is_inf()) result.set(n);
    }
    for (const auto e : graph.edges()) {
        const auto [src, dst] = graph.edge(e);
        if (graph.edge_cost(e, choices[src], choices[dst]).is_inf()) result.set(src).set(dst);
    }
    return result;
}

namespace {

Solution finish(const Graph& graph, Solution solution) {
    if (!solution.feasible()) solution.conflicts = conflicts(graph, solution.choices);
    return solution;
}

} // namespace

Solution solve(const Graph& graph, fe::View<size_t> rn_priority) {
    Reducer reducer{graph, rn_priority};
    fe::Vector<Subst> stack;
    stack.reserve(graph.num_nodes());

    for (NodeId n = reducer.next(); n != Graph::npos; n = reducer.next())
        reducer.reduce(n, stack);

    Choices choices(reducer.slots(), 0);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        choices[it->node] = reducer.choose(*it, choices);

    const Cost cost = evaluate(graph, choices);
    return finish(
        graph,
        Solution{.choices = std::move(choices), .cost = cost, .conflicts = {}, .statistics = reducer.statistics()});
}

namespace {

/// Mixed-radix layout of the assignment space; digit n belongs to node n.
struct Enumeration {
    fe::Vector<size_t, 16> options;
    fe::Vector<size_t, 16> place;
    size_t combinations = 1;
};

std::optional<Enumeration> make_enumeration(const Graph& graph, size_t max_combinations) {
    Enumeration enumeration;
    for (const auto n : graph.nodes())
        enumeration.options.emplace_back(graph.num_options(n));
    enumeration.place.resize(enumeration.options.size());

    size_t combinations = 1;
    for (const auto options : enumeration.options) {
        if (combinations > max_combinations / options) return std::nullopt;
        combinations *= options;
    }
    enumeration.combinations = combinations;

    size_t weight = 1;
    for (size_t i = 0; i < enumeration.options.size(); ++i) {
        enumeration.place[i] = weight;
        weight *= enumeration.options[i];
    }
    return enumeration;
}

/// The smallest index wins ties so that the result does not depend on the partitioning.
Solution search_range(const Graph& graph, const Enumeration& enumeration, size_t begin, size_t end) {
    Choices choices(enumeration.options.size(), 0);
    for (NodeId n = 0; n < choices.size(); ++n)
        choices[n] = (begin / enumeration.place[n]) % enumeration.options[n];

    Solution best{.choices = choices, .cost = Cost::inf(), .conflicts = {}, .statistics = {}};
    for (size_t index = begin; index < end; ++index) {
        const Cost cost = evaluate(graph, choices);
        if (cost < best.cost) {
            best.choices = choices;
            best.cost    = cost;
        }
        for (NodeId n = 0; n < choices.size(); ++n) {
            if (++choices[n] < enumeration.options[n]) break;
            choices[n] = 0;
        }
    }
    return best;
}

} // namespace

std::optional<Solution> solve_exhaustive(const Graph& graph, size_t max_combinations, unsigned num_threads) {
    const auto made = make_enumeration(graph, max_combinations);
    if (!made) return std::nullopt;
    const Enumeration& enumeration = *made;
    if (enumeration.options.empty()) return Solution{};

    unsigned requested = num_threads;
    if (requested == 0) requested = std::thread::hardware_concurrency();
    if (requested == 0) requested = 1;
    const size_t workers = std::min<size_t>(requested, enumeration.combinations);

    if (workers <= 1) return finish(graph, search_range(graph, enumeration, 0, enumeration.combinations));

    fe::Vector<Solution> partials(workers);
    {
        fe::Vector<std::jthread> threads;
        threads.reserve(workers);

        const size_t base  = enumeration.combinations / workers;
        const size_t extra = enumeration.combinations % workers;
        size_t begin       = 0;
        for (size_t i = 0; i < workers; ++i) {
            const size_t end = begin + base + (i < extra ? 1 : 0);
            threads.emplace_back([&graph, &enumeration, &partials, i, begin, end] {
                partials[i] = search_range(graph, enumeration, begin, end);
            });
            begin = end;
        }
    }

    // Ranges ascend, so the first minimum is the one with the smallest index.
    return finish(graph, std::move(*std::ranges::min_element(partials, {}, &Solution::cost)));
}

} // namespace pbqp
