#include "pbqp/dot.h"

#include <cassert>

#include <print>

namespace pbqp {

namespace {

constexpr const char* kChosen = R"( bgcolor="palegreen")";

void print_cell(std::ostream& out, Cost cost, bool chosen) {
    if (cost == kInfinity)
        std::print(out, "<td{}>&#8734;</td>", chosen ? kChosen : "");
    else
        std::print(out, "<td{}>{}</td>", chosen ? kChosen : "", cost);
}

} // namespace

void write_dot(std::ostream& out, const Graph& graph, fe::View<OptionId> choices) {
    const bool highlight = !choices.empty();
    for ([[maybe_unused]] const auto n : graph.nodes())
        assert((!highlight || n < choices.size()) && "write_dot: choices must cover every node id");

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
        const auto [src, dst] = graph.edge(e);
        const size_t columns  = graph.num_options(dst);
        std::print(
            out,
            R"(    n{} -- n{} [label=<<table border="0" cellborder="1" cellspacing="0"><tr><td colspan="{}">n{} &#215; n{}</td></tr>)",
            src, dst, columns, src, dst);
        for (OptionId i = 0; i < graph.num_options(src); ++i) {
            std::print(out, "<tr>");
            for (OptionId j = 0; j < columns; ++j)
                print_cell(out, graph.edge_cost(e, i, j), highlight && choices[src] == i && choices[dst] == j);
            std::print(out, "</tr>");
        }
        std::println(out, "</table>>];");
    }

    std::println(out, "}}");
}

} // namespace pbqp
