#include "pbqp/graph.h"

#include <algorithm>
#include <ostream>
#include <string_view>
#include <utility>

namespace pbqp {

std::ostream& operator<<(std::ostream& out, Cost cost) { return cost.is_inf() ? out << "inf" : out << cost.value(); }

NodeId Graph::add(fe::View<Cost> costs) {
    assert((!costs.empty()) && "add: a PBQP node needs at least one option");
    nodes_.emplace_back(Node{.costs = {costs.begin(), costs.end()}, .edges = {}});
    return nodes_.size() - 1;
}

EdgeId Graph::link(NodeId m, NodeId n, MatrixView costs) {
    assert((m < num_nodes() && n < num_nodes()) && "link: unknown node");
    assert((m != n) && "link: self edges are not supported, fold them into the node cost vector instead");
    assert((edge(m, n) == npos) && "link: an edge between these nodes already exists, merge the matrices instead");
    assert((costs.rows() == num_options(m) && costs.cols() == num_options(n))
           && "link: cost matrix has the wrong shape");

    edges_.emplace_back(Edge{.src = m, .dst = n, .matrix = intern(costs)});
    auto e = edges_.size() - 1;
    nodes_[m].edges.emplace_back(e);
    nodes_[n].edges.emplace_back(e);
    return e;
}

EdgeId Graph::add_edge_costs(NodeId m, NodeId n, MatrixView costs) {
    const EdgeId e = edge(m, n);
    if (e == npos) return link(m, n, costs);

    Matrix sum(edge_matrix(e));
    const MatrixSpan from_m = edges_[e].src == m ? sum.span() : sum.span().transposed();
    from_m += costs;
    set_edge_costs(e, sum);
    return e;
}

void Graph::set_node_costs(NodeId n, fe::View<Cost> costs) {
    assert((costs.size() == num_options(n)) && "set_node_costs: option count must not change");
    std::ranges::copy(costs, nodes_[n].costs.begin());
}

void Graph::set_edge_costs(EdgeId e, MatrixView costs) {
    assert((costs.rows() == edge_matrix(e).rows() && costs.cols() == edge_matrix(e).cols())
           && "set_edge_costs: cost matrix has the wrong shape");
    edges_[e].matrix = intern(costs);
}

EdgeId Graph::edge(NodeId m, NodeId n) const {
    assert((m < num_nodes() && n < num_nodes()) && "edge: unknown node");
    if (nodes_[n].edges.size() < nodes_[m].edges.size()) std::swap(m, n);
    for (const auto e : nodes_[m].edges) {
        const Edge& candidate = edges_[e];
        if (candidate.src == n || candidate.dst == n) return e;
    }
    return npos;
}

Graph::MatrixId Graph::intern(MatrixView matrix) {
    if (!matrix.row_major()) return intern(Matrix(matrix));
    const fe::View<Cost> costs(matrix.data(), matrix.size());
    const auto bytes  = std::string_view(reinterpret_cast<const char*>(costs.data()), costs.size() * sizeof(Cost));
    const size_t hash = ankerl::unordered_dense::hash<std::string_view>{}(bytes);

    const Probe probe{costs, hash};
    auto i = matrices_.find(probe);
    if (i == matrices_.end()) i = matrices_.emplace(probe).first;
    return static_cast<MatrixId>(i - matrices_.begin());
}

} // namespace pbqp
