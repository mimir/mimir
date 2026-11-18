#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <iosfwd>
#include <limits>
#include <ranges>
#include <type_traits>

#include <ankerl/unordered_dense.h>
#include <fe/span.h>
#include <fe/vector.h>

namespace pbqp {

/// @name Ids
/// Node and edge ids are dense and stable: nothing is ever removed from a Graph.
///@{
using NodeId   = size_t; ///< Identifier of a node in a Graph.
using EdgeId   = size_t; ///< Identifier of an edge in a Graph.
using OptionId = size_t; ///< Index of an option within a node's cost vector.
///@}

/// A saturating cost; Cost::inf forbids an option or option pair.
class Cost {
public:
    using Rep = int32_t;

    constexpr Cost() = default;
    constexpr Cost(Rep value)
        : value_(value) {}

    [[nodiscard]] static constexpr Cost inf() { return std::numeric_limits<Rep>::max(); }
    [[nodiscard]] static constexpr Cost min() { return std::numeric_limits<Rep>::min(); }

    [[nodiscard]] constexpr Rep value() const { return value_; }
    [[nodiscard]] constexpr bool is_inf() const { return value_ == std::numeric_limits<Rep>::max(); }
    constexpr explicit operator Rep() const { return value_; }

    friend constexpr auto operator<=>(Cost, Cost) = default;

    /// Any sign; saturates and Cost::inf absorbs.
    friend constexpr Cost operator+(Cost lhs, Cost rhs) {
        if (lhs.is_inf() || rhs.is_inf()) return inf();
        const auto sum = int64_t{lhs.value_} + rhs.value_;
        return static_cast<Rep>(std::clamp<int64_t>(sum, min().value_, inf().value_));
    }

    constexpr Cost& operator+=(Cost rhs) { return *this = *this + rhs; }

    friend std::ostream& operator<<(std::ostream&, Cost); ///< Prints Cost::inf as `inf`.

private:
    Rep value_ = 0;
};

/// @name PBQP Vectors & Matrices
/// PBQP vectors and matrices are small, so they live inline; a Matrix views such storage row-major.
///@{
using Costs   = fe::Vector<Cost, 4>;
using Edges   = fe::Vector<EdgeId, 4>;
using Choices = fe::Vector<OptionId, 8>; ///< One option per node, indexed by node id.
///@}

/// A row-major matrix of costs over storage owned elsewhere.
template<class T>
requires std::same_as<std::remove_cv_t<T>, Cost> class BasicMatrix {
public:
    constexpr BasicMatrix() = default;
    constexpr BasicMatrix(T* data, size_t rows, size_t cols)
        : data_(data)
        , rows_(rows)
        , cols_(cols) {}

    template<class U>
    requires(std::same_as<T, const Cost> && std::same_as<U, Cost>) constexpr BasicMatrix(const BasicMatrix<U>& other)
        : data_(other.data())
        , rows_(other.rows())
        , cols_(other.cols()) {}

    [[nodiscard]] constexpr T& operator[](size_t row, size_t col) {
        assert(row < rows_ && col < cols_);
        return data_[row * cols_ + col];
    }
    [[nodiscard]] constexpr const Cost& operator[](size_t row, size_t col) const {
        assert(row < rows_ && col < cols_);
        return data_[row * cols_ + col];
    }

    [[nodiscard]] constexpr size_t rows() const { return rows_; }
    [[nodiscard]] constexpr size_t cols() const { return cols_; }
    [[nodiscard]] constexpr size_t size() const { return rows_ * cols_; }

    [[nodiscard]] constexpr T* data() const { return data_; }
    [[nodiscard]] constexpr T* row(size_t row) const {
        assert(row < rows_);
        return data_ + row * cols_;
    }
    [[nodiscard]] constexpr T* begin() const { return data_; }
    [[nodiscard]] constexpr T* end() const { return data_ + size(); }

    constexpr BasicMatrix& operator+=(BasicMatrix<const Cost> rhs) requires std::same_as<T, Cost> {
        assert(rows_ == rhs.rows() && cols_ == rhs.cols());
        for (size_t i = 0; i < size(); ++i)
            data_[i] += rhs.data()[i];
        return *this;
    }

    /// Adds the transpose of `rhs`, i.e. `(*this)[r, c] += rhs[c, r]`.
    constexpr void add_transposed(BasicMatrix<const Cost> rhs) requires std::same_as<T, Cost> {
        assert(rows_ == rhs.cols() && cols_ == rhs.rows());
        assert((size() <= 1 || data_ != rhs.data()) && "add_transposed: rhs aliases this");
        for (size_t r = 0; r < rows_; ++r)
            for (size_t c = 0; c < cols_; ++c)
                (*this)[r, c] += rhs[c, r];
    }

    /// Replaces every entry with the transpose of `src`.
    constexpr void transpose_from(BasicMatrix<const Cost> src) requires std::same_as<T, Cost> {
        assert(rows_ == src.cols() && cols_ == src.rows());
        assert((size() <= 1 || data_ != src.data()) && "transpose_from: src aliases this");
        for (size_t r = 0; r < rows_; ++r)
            for (size_t c = 0; c < cols_; ++c)
                (*this)[r, c] = src[c, r];
    }

    // `&` so that erasing on an rvalue cannot compact the storage yet drop the shrunken dimensions.
    constexpr void erase_row(size_t row) & requires std::same_as<T, Cost> {
        assert(row < rows_);
        std::copy_n(data_ + (row + 1) * cols_, (rows_ - row - 1) * cols_, data_ + row * cols_);
        --rows_;
    }

    constexpr void erase_col(size_t col) & requires std::same_as<T, Cost> {
        assert(col < cols_);
        T* out = data_;
        for (size_t r = 0; r < rows_; ++r) {
            const Cost* const in = data_ + r * cols_;
            out                  = std::copy(in, in + col, out);
            out                  = std::copy(in + col + 1, in + cols_, out);
        }
        --cols_;
    }

private:
    T* data_     = nullptr;
    size_t rows_ = 0;
    size_t cols_ = 0;
};

using Matrix     = BasicMatrix<Cost>;
using MatrixView = BasicMatrix<const Cost>;

/// A PBQP problem graph.
///
/// Every node carries a cost vector with one entry per option. Every edge
/// carries a cost matrix indexed by the options of its endpoints:
///
///     matrix[src_option, dst_option]
///
/// The total cost of an assignment `x` is
///
///     sum_n c_n(x_n) + sum_{mn} M_mn(x_m, x_n)
///
/// A node holds at most one edge to any other node, so parallel edges have to be merged by summing their matrices
/// first. A Graph only grows; ids are handed out consecutively from 0.
class Graph {
public:
    struct Ends {
        NodeId src = 0;
        NodeId dst = 0;
    };

    static constexpr EdgeId npos = static_cast<EdgeId>(-1);

    /// @name Construction
    ///@{
    /// @name Add Nodes, Edges, and Costs
    ///@{

    /// Add a node with the given cost vector.
    /// The vector must not be empty.
    NodeId add(fe::View<Cost> costs);

    /// Link `m` and `n` with a row-major num_options(m) x num_options(n) matrix.
    /// Requires distinct nodes that are not linked yet.
    EdgeId link(NodeId m, NodeId n, fe::View<Cost> costs);

    /// Like link, but sums `costs` onto an existing edge between `m` and `n`, whichever way it points.
    /// Sums saturate and Cost::inf absorbs.
    EdgeId add_edge_costs(NodeId m, NodeId n, fe::View<Cost> costs);

    void set_node_costs(NodeId n, fe::View<Cost> costs); ///< Replace a cost vector; the option count must not change.
    void set_edge_costs(EdgeId e, fe::View<Cost> costs); ///< Replace a cost matrix; the size must not change.
    ///@}

    /// @name Getters
    ///@{
    [[nodiscard]] fe::View<Cost> node_costs(NodeId n) const { return checked_node(n).costs; }

    /// Row-major with the src option as the row.
    [[nodiscard]] fe::View<Cost> edge_costs(EdgeId e) const {
        const MatrixView matrix = edge_matrix(e);
        return {matrix.data(), matrix.size()};
    }

    /// Row-major with the src option as the row.
    [[nodiscard]] MatrixView edge_matrix(EdgeId e) const {
        const Edge& edge   = checked_edge(e);
        const Costs& costs = matrix(edge.matrix);
        return {costs.data(), num_options(edge.src), num_options(edge.dst)};
    }

    /// Matrix entry of `e`, addressed as (src option, dst option).
    [[nodiscard]] Cost edge_cost(EdgeId e, OptionId src_option, OptionId dst_option) const {
        return edge_matrix(e)[src_option, dst_option];
    }

    [[nodiscard]] size_t num_options(NodeId n) const { return node_costs(n).size(); }

    [[nodiscard]] fe::View<EdgeId> incident_edges(NodeId n) const { return checked_node(n).edges; }
    [[nodiscard]] Ends edge(EdgeId e) const {
        const Edge& edge = checked_edge(e);
        return {.src = edge.src, .dst = edge.dst};
    }
    [[nodiscard]] EdgeId edge(NodeId m, NodeId n) const; ///< The edge between `m` and `n`, or npos.

    [[nodiscard]] auto nodes() const { return std::views::iota(NodeId{0}, num_nodes()); } ///< All node ids.
    [[nodiscard]] auto edges() const { return std::views::iota(EdgeId{0}, num_edges()); } ///< All edge ids.

    [[nodiscard]] size_t num_nodes() const { return nodes_.size(); }
    [[nodiscard]] size_t num_edges() const { return edges_.size(); }
    /// Number of distinct edge matrices ever linked or set; edges with equal matrices share one copy.
    [[nodiscard]] size_t num_matrices() const { return matrices_.size(); }
    ///@}

private:
    using MatrixId = size_t;

    /// Costs that are not interned yet, together with their hash.
    struct Probe {
        fe::View<Cost> costs;
        size_t hash;
    };

    struct PooledMatrix {
        explicit PooledMatrix(const Probe& probe)
            : costs(probe.costs.begin(), probe.costs.end())
            , hash(probe.hash) {}

        Costs costs;
        size_t hash;
    };

    struct PooledMatrixHash {
        using is_transparent = void;
        using is_avalanching = void;
        size_t operator()(const PooledMatrix& matrix) const noexcept { return matrix.hash; }
        size_t operator()(const Probe& probe) const noexcept { return probe.hash; }
    };

    struct PooledMatrixEq {
        using is_transparent = void;
        bool operator()(const PooledMatrix& a, const PooledMatrix& b) const noexcept { return a.costs == b.costs; }
        bool operator()(const Probe& probe, const PooledMatrix& matrix) const noexcept {
            return std::ranges::equal(matrix.costs, probe.costs);
        }
        bool operator()(const PooledMatrix& matrix, const Probe& probe) const noexcept {
            return (*this)(probe, matrix);
        }
    };

    struct Node {
        Costs costs;
        Edges edges; // incident edges, no duplicates
    };

    struct Edge {
        NodeId src      = 0;
        NodeId dst      = 0;
        MatrixId matrix = npos;
    };

    [[nodiscard]] const Graph::Node& checked_node(NodeId n) const {
        assert((n < num_nodes()) && "node: unknown node");
        return nodes_[n];
    }

    [[nodiscard]] const Graph::Edge& checked_edge(EdgeId e) const {
        assert((e < num_edges()) && "edge: unknown edge");
        return edges_[e];
    }

    [[nodiscard]] MatrixId intern(fe::View<Cost> costs);
    [[nodiscard]] const Costs& matrix(MatrixId id) const { return matrices_.values()[id].costs; }

    fe::Vector<Node> nodes_;
    fe::Vector<Edge> edges_;
    // Never erased, so positions are ids.
    ankerl::unordered_dense::set<PooledMatrix, PooledMatrixHash, PooledMatrixEq> matrices_;
};

} // namespace pbqp
