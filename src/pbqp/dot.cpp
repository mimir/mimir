#include "pbqp/dot.h"

#include <cassert>

#include <print>

namespace pbqp {

namespace {

constexpr const char* kChosen = R"( bgcolor="palegreen")";

void print_cell(std::ostream& out, Cost cost, bool chosen) {
    const char* const attr = chosen ? kChosen : "";
    if (cost.is_inf())
        std::print(out, "<td{}>&#8734;</td>", attr);
    else
        std::print(out, "<td{}>{}</td>", attr, cost.value());
}

} // namespace

void write_dot(std::ostream& out, const Graph& graph, fe::View<OptionId> choices) {
    const bool highlight = !choices.empty();
    assert((!highlight || choices.size() >= graph.num_nodes()) && "write_dot: choices must cover every node id");

    std::println(out, "graph pbqp {{");
    std::println(out, "    node [shape=plain];");
    std::println(out, "    edge [fontsize=10];");

    for (const auto n : graph.nodes()) {
        const auto costs = graph.node_costs(n);
        std::print(
            out,
            R"(    n{} [label=<<table border="0" cellborder="1" cellspacing="0"><tr><td colspan="{}"><b>n{}</b></td></tr><tr>)",
            n, costs.size(), n);
        for (OptionId i = 0; i < costs.size(); ++i)
            print_cell(out, costs[i], highlight && choices[n] == i);
        std::println(out, "</tr></table>>];");
    }

    for (const auto e : graph.edges()) {
        const auto [src, dst]   = graph.edge(e);
        const MatrixView matrix = graph.edge_matrix(e);
        std::print(
            out,
            R"(    n{} -- n{} [label=<<table border="0" cellborder="1" cellspacing="0"><tr><td colspan="{}">n{} &#215; n{}</td></tr>)",
            src, dst, matrix.cols(), src, dst);
        for (OptionId i = 0; i < matrix.rows(); ++i) {
            std::print(out, "<tr>");
            for (OptionId j = 0; j < matrix.cols(); ++j)
                print_cell(out, matrix[i, j], highlight && choices[src] == i && choices[dst] == j);
            std::print(out, "</tr>");
        }
        std::println(out, "</table>>];");
    }

    std::println(out, "}}");
}

} // namespace pbqp
