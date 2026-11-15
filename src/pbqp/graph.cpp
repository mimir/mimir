#include "pbqp/graph.h"

#include <cassert>

#include <algorithm>
#include <string_view>
#include <utility>

#include "cost.h"

namespace pbqp {

NodeId Graph::add(fe::View<Cost> costs) {
    assert((!costs.empty()) && "add: a PBQP node needs at least one option");
    nodes_.emplace_back(Node{
        .costs = {costs.begin(), costs.end()},
        .edges = {}
    });
    auto n = nodes_.size() - 1;
    node_mask_.set(n);
    return n;
}

EdgeId Graph::link(NodeId m, NodeId n, fe::View<Cost> costs) {
    assert((has_node(m) && has_node(n)) && "link: unknown node");
    assert((m != n) && "link: self edges are not supported, fold them into the node cost vector instead");
    assert((edge(m, n) == npos) && "link: an edge between these nodes already exists, merge the matrices instead");
    assert((costs.size() == num_options(m) * num_options(n)) && "link: cost matrix has the wrong size");

    edges_.emplace_back(Edge{.src = m, .dst = n, .matrix = intern(costs)});
    auto e = edges_.size() - 1;
    nodes_[m].edges.emplace_back(e);
    nodes_[n].edges.emplace_back(e);
    edge_mask_.set(e);
    return e;
}

EdgeId Graph::add_edge_costs(NodeId m, NodeId n, fe::View<Cost> costs) {
    const EdgeId e = edge(m, n);
    if (e == npos) return link(m, n, costs);
    assert((costs.size() == num_options(m) * num_options(n)) && "add_edge_costs: cost matrix has the wrong size");

    const size_t rows   = num_options(m);
    const size_t cols   = num_options(n);
    const bool same_way = edges_[e].src == m;
    Costs sum(edge_costs(e).begin(), edge_costs(e).end());
    for (size_t r = 0; r < rows; ++r)
        for (size_t c = 0; c < cols; ++c) {
            Cost& entry = sum[same_way ? r * cols + c : c * rows + r];
            entry       = add_signed(entry, costs[r * cols + c]);
        }
    set_edge_costs(e, sum);
    return e;
}

void Graph::unlink(EdgeId e) {
    assert((has_edge(e)) && "unlink: unknown edge");

    Edge& removed = edges_[e];
    edge_mask_.clear(e);
    release(removed.matrix);
    removed.matrix = npos;
    for (const auto endpoint : {removed.src, removed.dst}) {
        auto& incident = nodes_[endpoint].edges;
        erase(incident, e);
    }
}

void Graph::remove(NodeId n) {
    assert((has_node(n)) && "remove: unknown node");

    const Edges incident = nodes_[n].edges;
    for (const auto e : incident)
        unlink(e);
    node_mask_.clear(n);
}

bool Graph::has_node(NodeId n) const { return node_mask_.test(n); }

bool Graph::has_edge(EdgeId e) const { return edge_mask_.test(e); }

const Graph::Node& Graph::checked_node(NodeId n) const {
    assert((has_node(n)) && "node: unknown or removed node");
    return nodes_[n];
}

const Graph::Edge& Graph::checked_edge(EdgeId e) const {
    assert((has_edge(e)) && "edge: unknown or removed edge");
    return edges_[e];
}

fe::View<Cost> Graph::node_costs(NodeId n) const { return checked_node(n).costs; }

fe::View<EdgeId> Graph::incident_edges(NodeId n) const { return checked_node(n).edges; }

Graph::Ends Graph::edge(EdgeId e) const {
    const Edge& edge = checked_edge(e);
    return {.src = edge.src, .dst = edge.dst};
}

fe::View<Cost> Graph::edge_costs(EdgeId e) const { return matrices_[checked_edge(e).matrix].costs; }

void Graph::set_node_costs(NodeId n, fe::View<Cost> costs) {
    assert((costs.size() == num_options(n)) && "set_node_costs: option count must not change");
    std::ranges::copy(costs, nodes_[n].costs.begin());
}

void Graph::set_edge_costs(EdgeId e, fe::View<Cost> costs) {
    assert((costs.size() == edge_costs(e).size()) && "set_edge_costs: cost matrix has the wrong size");
    const MatrixId old = edges_[e].matrix;
    edges_[e].matrix   = intern(costs);
    release(old);
}

Cost Graph::edge_cost(EdgeId e, OptionId src_option, OptionId dst_option) const {
    const Edge& edge = checked_edge(e);
    return matrices_[edge.matrix].costs[src_option * num_options(edge.dst) + dst_option];
}

EdgeId Graph::edge(NodeId m, NodeId n) const {
    assert((has_node(m) && has_node(n)) && "edge: unknown node");
    if (nodes_[n].edges.size() < nodes_[m].edges.size()) std::swap(m, n);
    for (const auto e : nodes_[m].edges) {
        const Edge& candidate = edges_[e];
        if (candidate.src == n || candidate.dst == n) return e;
    }
    return npos;
}

Graph::MatrixId Graph::intern(fe::View<Cost> costs) {
    const auto bytes  = std::string_view(reinterpret_cast<const char*>(costs.data()), costs.size() * sizeof(Cost));
    const size_t hash = ankerl::unordered_dense::hash<std::string_view>{}(bytes);

    const auto i = hash2matrix_.try_emplace(hash, npos).first;
    for (MatrixId id = i->second; id != npos; id = matrices_[id].next) {
        if (std::ranges::equal(matrices_[id].costs, costs)) {
            ++matrices_[id].refs;
            return id;
        }
    }

    MatrixId id = free_matrix_;
    if (id == npos) {
        id = matrices_.size();
        matrices_.emplace_back();
    } else {
        free_matrix_ = matrices_[id].next;
    }
    matrices_[id] = Matrix{
        .costs = {costs.begin(), costs.end()},
        .hash  = hash,
        .refs  = 1,
        .next  = i->second
    };
    i->second = id;
    ++num_matrices_;
    return id;
}

void Graph::release(MatrixId id) {
    Matrix& matrix = matrices_[id];
    if (--matrix.refs != 0) return;

    const auto i = hash2matrix_.find(matrix.hash);
    if (i->second == id) {
        if (matrix.next == npos)
            hash2matrix_.erase(i);
        else
            i->second = matrix.next;
    } else {
        MatrixId prev = i->second;
        while (matrices_[prev].next != id)
            prev = matrices_[prev].next;
        matrices_[prev].next = matrix.next;
    }
    matrix       = Matrix{.costs = {}, .hash = 0, .refs = 0, .next = free_matrix_};
    free_matrix_ = id;
    --num_matrices_;
}

} // namespace pbqp
