#pragma once

#include <cstddef>
#include <cstdint>

#include <limits>

#include <ankerl/unordered_dense.h>
#include <fe/bitset.h>
#include <fe/span.h>
#include <fe/vector.h>

namespace pbqp {

/// Identifier of a node in a Graph. Ids are stable for the lifetime of the graph.
using NodeId = size_t;
/// Identifier of an edge in a Graph.
using EdgeId = size_t;
/// Index of an option within a node's cost vector.
using OptionIndex = size_t;

/// Compiler PBQP costs are small integers; totals use CostSum.
using Cost    = std::int32_t;
using CostSum = std::int64_t;

/// Forbids an option or option pair; every total that contains it is kInfiniteSum.
inline constexpr Cost kInfinity       = std::numeric_limits<Cost>::max();
inline constexpr CostSum kInfiniteSum = std::numeric_limits<CostSum>::max();

/// PBQP vectors and matrices are small, so they live inline.
using Costs = fe::Vector<Cost, 4>;
using Edges = fe::Vector<EdgeId, 4>;
/// One option per live node, indexed by node id.
using Choices = fe::Vector<OptionIndex, 8>;

/// A PBQP problem graph.
///
/// Every node carries a cost vector with one entry per option. Every edge
/// carries a cost matrix indexed by the options of its endpoints:
///
///     costs[src_option * num_options(dst) + dst_option]
///
/// The total cost of an assignment `x` is
///
///     sum_n c_n(x_n) + sum_{mn} M_mn(x_m, x_n)
///
/// Node and edge ids are stable: removing an element leaves a hole that is
/// skipped by nodes()/edges() but never reused. A node holds at most one edge
/// to any other node, so parallel edges have to be merged by summing their
/// matrices first.
class Graph {
public:
    static constexpr EdgeId npos = static_cast<EdgeId>(-1);

    /// Add a node with the given cost vector. The vector must not be empty.
    NodeId add(fe::View<Cost> costs);

    /// Link `m` and `n` with a row-major num_options(m) x num_options(n) matrix.
    /// Requires distinct nodes that are not linked yet.
    EdgeId link(NodeId m, NodeId n, fe::View<Cost> costs);
    /// Like link, but sums `costs` onto an existing edge between `m` and `n`, whichever way it points.
    /// Sums saturate and kInfinity absorbs.
    EdgeId add_edge_costs(NodeId m, NodeId n, fe::View<Cost> costs);

    void unlink(EdgeId e);
    void remove(NodeId n);

    [[nodiscard]] bool has_node(NodeId n) const;
    [[nodiscard]] bool has_edge(EdgeId e) const;

    [[nodiscard]] fe::View<Cost> node_costs(NodeId n) const;
    [[nodiscard]] fe::View<EdgeId> incident_edges(NodeId n) const;
    [[nodiscard]] size_t num_options(NodeId n) const { return node_costs(n).size(); }

    struct Endpoints {
        NodeId src = 0;
        NodeId dst = 0;
    };

    [[nodiscard]] Endpoints edge(EdgeId e) const;
    /// Row-major with the src option as the row.
    [[nodiscard]] fe::View<Cost> edge_costs(EdgeId e) const;

    /// Replace a cost vector; the option count must not change.
    void set_node_costs(NodeId n, fe::View<Cost> costs);
    /// Replace a cost matrix; the size must not change.
    void set_edge_costs(EdgeId e, fe::View<Cost> costs);

    /// Matrix entry of `e`, addressed as (src option, dst option).
    [[nodiscard]] Cost edge_cost(EdgeId e, OptionIndex src_option, OptionIndex dst_option) const;

    /// The edge between `m` and `n`, or npos.
    [[nodiscard]] EdgeId edge(NodeId m, NodeId n) const;

    /// Ids of live nodes and edges in ascending order.
    [[nodiscard]] const fe::Bitset& nodes() const { return node_mask_; }
    [[nodiscard]] const fe::Bitset& edges() const { return edge_mask_; }

    [[nodiscard]] size_t num_nodes() const { return node_mask_.count(); }
    [[nodiscard]] size_t num_edges() const { return edge_mask_.count(); }
    /// Number of distinct edge matrices; edges with equal matrices share one copy.
    [[nodiscard]] size_t num_matrices() const { return num_matrices_; }

private:
    using MatrixId = size_t;

    /// Keys are already avalanched, so this hash only has to stop ankerl from mixing them again.
    struct Prehashed {
        using is_avalanching = void;
        size_t operator()(size_t hash) const noexcept { return hash; }
    };

    struct Matrix {
        Costs costs;
        size_t hash   = 0;
        size_t refs   = 0;
        MatrixId next = npos; // next matrix with the same hash while referenced, next free slot otherwise
    };

    struct Node {
        Costs costs;
        Edges edges; // incident live edges, no duplicates
    };

    struct Edge {
        NodeId src      = 0;
        NodeId dst      = 0;
        MatrixId matrix = npos;
    };

    [[nodiscard]] const Node& checked_node(NodeId n) const;
    [[nodiscard]] const Edge& checked_edge(EdgeId e) const;

    [[nodiscard]] MatrixId intern(fe::View<Cost> costs);
    void release(MatrixId id);

    fe::Vector<Node, 2> nodes_;
    fe::Vector<Edge, 2> edges_;
    fe::Bitset node_mask_;
    fe::Bitset edge_mask_;
    fe::Vector<Matrix, 2> matrices_;
    ankerl::unordered_dense::map<size_t, MatrixId, Prehashed> hash2matrix_;
    MatrixId free_matrix_ = npos;
    size_t num_matrices_  = 0;
};

} // namespace pbqp
