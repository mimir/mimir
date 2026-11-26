#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <initializer_list>
#include <iosfwd>
#include <limits>
#include <ranges>
#include <type_traits>
#include <utility>

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

/// A view of a cost matrix over row-major storage owned elsewhere; transposed() views the same storage column-major.
template<class T>
requires std::same_as<std::remove_cv_t<T>, Cost> class BasicMatrix {
public:
    constexpr BasicMatrix() = default;
    constexpr BasicMatrix(T* data, size_t rows, size_t cols)
        : BasicMatrix(data, rows, cols, true) {}

    template<class U>
    requires(std::same_as<T, const Cost> && std::same_as<U, Cost>) constexpr BasicMatrix(const BasicMatrix<U>& other)
        : BasicMatrix(other.data(), other.rows(), other.cols(), other.row_major()) {}

    [[nodiscard]] constexpr T& operator[](size_t row, size_t col) const {
        assert(row < rows_ && col < cols_);
        return data_[row * row_stride_ + col * col_stride_];
    }

    [[nodiscard]] constexpr size_t rows() const { return rows_; }
    [[nodiscard]] constexpr size_t cols() const { return cols_; }
    [[nodiscard]] constexpr size_t size() const { return rows_ * cols_; }

    /// Whether rows are contiguous, which also holds for a transposed single row or column.
    [[nodiscard]] constexpr bool row_major() const { return col_stride_ == 1; }
    [[nodiscard]] constexpr BasicMatrix transposed() const { return {data_, cols_, rows_, !row_major()}; }

    /// @name Storage
    /// Iteration covers the storage in memory order, which is row-major only if row_major().
    ///@{
    [[nodiscard]] constexpr T* data() const { return data_; }
    [[nodiscard]] constexpr T* begin() const { return data_; }
    [[nodiscard]] constexpr T* end() const { return data_ + size(); }
    [[nodiscard]] constexpr T* row(size_t row) const {
        assert(row < rows_ && row_major());
        return data_ + row * row_stride_;
    }
    ///@}

    constexpr const BasicMatrix& operator+=(BasicMatrix<const Cost> rhs) const requires(!std::is_const_v<T>) {
        assert(rows_ == rhs.rows() && cols_ == rhs.cols());
        assert((size() <= 1 || data_ != rhs.data() || row_major() == rhs.row_major()) && "+=: rhs aliases this");
        for (size_t r = 0; r < rows_; ++r)
            for (size_t c = 0; c < cols_; ++c)
                (*this)[r, c] += rhs[r, c];
        return *this;
    }

    constexpr void copy_from(BasicMatrix<const Cost> src) const requires(!std::is_const_v<T>) {
        assert(rows_ == src.rows() && cols_ == src.cols());
        assert((size() <= 1 || data_ != src.data() || row_major() == src.row_major()) && "copy_from: src aliases this");
        for (size_t r = 0; r < rows_; ++r)
            for (size_t c = 0; c < cols_; ++c)
                (*this)[r, c] = src[r, c];
    }

    /// Erases the rows in `rows`, which must be sorted and distinct.
    /// The `&` ensures erasing an rvalue cannot compact the storage yet drop the shrunken dimensions.
    constexpr void erase_rows(fe::View<size_t> rows) & requires(!std::is_const_v<T>) {
        if (rows.empty()) return;
        assert(std::ranges::is_sorted(rows) && "erase_rows: rows not sorted");
        assert(std::ranges::adjacent_find(rows) == rows.end() && "erase_rows: duplicate row");
        assert(rows.back() < rows_ && "erase_rows: row out of range");
        if (row_major()) {
            size_t dst = 0;
            size_t src = 0;
            for (const size_t row : rows) {
                if (dst != src) std::copy_n(data_ + src * cols_, (row - src) * cols_, data_ + dst * cols_);
                dst += row - src;
                src = row + 1;
            }
            if (dst != src) std::copy_n(data_ + src * cols_, (rows_ - src) * cols_, data_ + dst * cols_);
            dst += rows_ - src;
            *this = BasicMatrix(data_, dst, cols_, true);
        } else {
            erase_storage_cols(rows);
            *this = BasicMatrix(data_, rows_ - rows.size(), cols_, false);
        }
    }

    /// Erases the columns in `cols`, which must be sorted and distinct.
    constexpr void erase_cols(fe::View<size_t> cols) & requires(!std::is_const_v<T>) {
        *this = transposed();
        erase_rows(cols);
        *this = transposed();
    }

private:
    constexpr BasicMatrix(T* data, size_t rows, size_t cols, bool row_major)
        : data_(data)
        , rows_(rows)
        , cols_(cols)
        , row_stride_(row_major || rows <= 1 || cols <= 1 ? cols : 1)
        , col_stride_(row_major || rows <= 1 || cols <= 1 ? 1 : rows) {}

    /// Erases the storage columns in `cols`, which are positions within each of the `cols_` rows of `rows_` entries.
    constexpr void erase_storage_cols(fe::View<size_t> cols) {
        T* out = data_;
        for (size_t r = 0; r < cols_; ++r) {
            const T* const base = data_ + r * rows_;
            const T* in         = base;
            for (const auto col : cols) {
                out = std::copy(in, base + col, out);
                in  = base + col + 1;
            }
            out = std::copy(in, base + rows_, out);
        }
    }

    T* data_           = nullptr;
    size_t rows_       = 0;
    size_t cols_       = 0;
    size_t row_stride_ = 0;
    size_t col_stride_ = 1;
};

using MatrixSpan = BasicMatrix<Cost>;
using MatrixView = BasicMatrix<const Cost>;

/// An owning row-major cost matrix; it views its own storage, so writes go through span().
class Matrix : public MatrixView {
public:
    Matrix() = default;
    Matrix(size_t rows, size_t cols, Cost fill = 0)
        : Matrix(Costs(rows * cols, fill), rows, cols) {}
    Matrix(size_t rows, size_t cols, std::initializer_list<Cost> costs)
        : Matrix(Costs(costs), rows, cols) {}
    /// Entry `(r, c)` is `f(r, c)`, generated in row-major order.
    template<class F>
    requires std::is_invocable_r_v<Cost, F&, size_t, size_t> Matrix(size_t rows, size_t cols, F f)
        : Matrix(Costs(rows * cols, [&f, cols](size_t i) -> Cost { return f(i / cols, i % cols); }), rows, cols) {}
    explicit Matrix(MatrixView view)
        : Matrix(view.rows(), view.cols(), [view](size_t r, size_t c) { return view[r, c]; }) {}

    Matrix(const Matrix& other)
        : Matrix(Costs(other.costs_), other.rows(), other.cols()) {}
    Matrix(Matrix&& other) noexcept
        : Matrix(std::move(other.costs_), other.rows(), other.cols()) {
        other.view() = {};
    }
    Matrix& operator=(Matrix other) noexcept {
        costs_       = std::move(other.costs_);
        view()       = {costs_.data(), other.rows(), other.cols()};
        other.view() = {};
        return *this;
    }

    [[nodiscard]] MatrixSpan span() { return {costs_.data(), rows(), cols()}; }

    Matrix& operator+=(MatrixView rhs) {
        span() += rhs;
        return *this;
    }

    friend bool operator==(const Matrix& a, const Matrix& b) {
        return a.rows() == b.rows() && a.cols() == b.cols() && a.costs_ == b.costs_;
    }

private:
    Matrix(Costs&& costs, size_t rows, size_t cols)
        : costs_(std::move(costs)) {
        assert(costs_.size() == rows * cols && "Matrix: wrong number of costs");
        view() = {costs_.data(), rows, cols};
    }

    MatrixView& view() { return *this; }

    Costs costs_;
};

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

    /// Link `m` and `n` with a num_options(m) × num_options(n) matrix.
    /// Requires distinct nodes that are not linked yet.
    EdgeId link(NodeId m, NodeId n, MatrixView costs);

    /// Like link, but sums `costs` onto an existing edge between `m` and `n`, whichever way it points.
    /// Sums saturate and Cost::inf absorbs.
    EdgeId add_edge_costs(NodeId m, NodeId n, MatrixView costs);

    void set_node_costs(NodeId n, fe::View<Cost> costs); ///< Replace a cost vector; the option count must not change.
    void set_edge_costs(EdgeId e, MatrixView costs);     ///< Replace a cost matrix, src options as rows.
    ///@}

    /// @name Getters
    ///@{
    [[nodiscard]] fe::View<Cost> node_costs(NodeId n) const { return checked_node(n).costs; }

    /// Row-major with the src option as the row.
    [[nodiscard]] MatrixView edge_matrix(EdgeId e) const {
        const Edge& edge   = checked_edge(e);
        const Costs& costs = matrix(edge.matrix);
        return {costs.data(), num_options(edge.src), num_options(edge.dst)};
    }

    /// The options of `from`, an end of `e`, are the rows.
    [[nodiscard]] MatrixView edge_matrix(EdgeId e, NodeId from) const {
        const auto [src, dst] = edge(e);
        assert((from == src || from == dst) && "edge_matrix: not an end of the edge");
        return from == src ? edge_matrix(e) : edge_matrix(e).transposed();
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

    [[nodiscard]] const Node& checked_node(NodeId n) const {
        assert((n < num_nodes()) && "node: unknown node");
        return nodes_[n];
    }

    [[nodiscard]] const Edge& checked_edge(EdgeId e) const {
        assert((e < num_edges()) && "edge: unknown edge");
        return edges_[e];
    }

    [[nodiscard]] MatrixId intern(MatrixView matrix);
    [[nodiscard]] const Costs& matrix(MatrixId id) const { return matrices_.values()[id].costs; }

    fe::Vector<Node> nodes_;
    fe::Vector<Edge> edges_;
    // Never erased, so positions are ids.
    ankerl::unordered_dense::set<PooledMatrix, PooledMatrixHash, PooledMatrixEq> matrices_;
};

} // namespace pbqp
