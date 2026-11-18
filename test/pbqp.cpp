#include <cstddef>
#include <cstdint>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <doctest/doctest.h>
#include <fe/vector.h>
#include <pbqp/dot.h>
#include <pbqp/graph.h>
#include <pbqp/json.h>
#include <pbqp/solver.h>

namespace {

TEST_CASE("single node picks cheapest option") {
    pbqp::Graph graph;
    const pbqp::NodeId n = graph.add({5, 1, 3});

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.choices[n] == 1);
    CHECK(solution.cost == 1);
}

TEST_CASE("independent nodes are independent") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({4, 2});
    const pbqp::NodeId b = graph.add({7, 8, 6});

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.choices[a] == 1);
    CHECK(solution.choices[b] == 2);
    CHECK(solution.cost == 8);
}

TEST_CASE("matrix indexes rows and columns") {
    std::array<pbqp::Cost, 6> cells = {1, 2, 3, 4, 5, 6};
    const pbqp::MatrixSpan matrix(cells.data(), 2, 3);
    CHECK(matrix.rows() == 2);
    CHECK(matrix.cols() == 3);
    CHECK(matrix.size() == 6);
    CHECK(matrix[0, 0] == 1);
    CHECK(matrix[1, 2] == 6);
    CHECK(matrix.row(1)[1] == 5);

    matrix[1, 2] = 7;
    CHECK(cells[5] == 7);
    CHECK(std::ranges::equal(matrix, std::array<pbqp::Cost, 6>{1, 2, 3, 4, 5, 7}));

    const pbqp::MatrixView view(cells.data(), 2, 3);
    CHECK(view[1, 2] == 7);

    const pbqp::Matrix owned(2, 3, [](size_t r, size_t c) { return pbqp::Cost(static_cast<int>(10 * r + c)); });
    CHECK(owned == pbqp::Matrix(2, 3, {0, 1, 2, 10, 11, 12}));
    CHECK(pbqp::Matrix(pbqp::MatrixView(owned).transposed()) == pbqp::Matrix(3, 2, {0, 10, 1, 11, 2, 12}));

    // Copies, moves, and assignments view their own storage.
    pbqp::Matrix copy = owned;
    CHECK(copy == owned);
    CHECK(copy.data() != owned.data());
    copy.span()[0, 0] = 5;
    CHECK(owned[0, 0] == 0);
    const pbqp::Matrix moved = std::move(copy);
    CHECK(moved[0, 0] == 5);
    copy = moved;
    CHECK(copy[1, 2] == 12);
    CHECK(copy.data() != moved.data());
    pbqp::Matrix wide(5, 5, 7);
    wide = pbqp::Matrix(2, 2, {1, 2, 3, 4});
    CHECK(wide == pbqp::Matrix(2, 2, {1, 2, 3, 4}));
}

TEST_CASE("matrix operations transpose and add") {
    const std::array<pbqp::Cost, 6> cells = {1, 2, 3, 4, 5, 6};
    const pbqp::MatrixView transposed     = pbqp::MatrixView(cells.data(), 2, 3).transposed();
    CHECK(transposed.rows() == 3);
    CHECK(!transposed.row_major());
    CHECK(transposed[0, 0] == 1);
    CHECK(transposed[1, 0] == 2);
    CHECK(transposed[2, 0] == 3);
    CHECK(transposed[0, 1] == 4);
    CHECK(transposed[2, 1] == 6);
    CHECK(transposed.transposed().row_major());
    CHECK(pbqp::MatrixView(cells.data(), 1, 6).transposed().row_major());

    pbqp::Matrix sum(2, 2, {10, 20, 30, 40});
    const pbqp::Matrix add(2, 2, {1, 2, 3, 4});
    sum += add;
    CHECK(sum == pbqp::Matrix(2, 2, {11, 22, 33, 44}));
    sum += pbqp::MatrixView(add).transposed();
    CHECK(sum == pbqp::Matrix(2, 2, {12, 25, 35, 48}));
}

TEST_CASE("matrix erases several rows and columns at once") {
    const std::array<pbqp::Cost, 9> cells       = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    constexpr std::array<size_t, 2> every_other = {0, 2};

    std::array<pbqp::Cost, 9> rows_cells = cells;
    pbqp::MatrixSpan rows(rows_cells.data(), 3, 3);
    rows.erase_rows(every_other);
    CHECK(rows.rows() == 1);
    CHECK(rows.cols() == 3);
    CHECK(std::ranges::equal(rows, std::array<pbqp::Cost, 3>{4, 5, 6}));

    // Erasing rows of the transposed view erases several storage columns.
    constexpr std::array<size_t, 2> first_rows = {0, 1};
    std::array<pbqp::Cost, 9> flipped_cells    = cells;
    pbqp::MatrixSpan flipped                   = pbqp::MatrixSpan(flipped_cells.data(), 3, 3).transposed();
    flipped.erase_rows(first_rows);
    CHECK(flipped.rows() == 1);
    CHECK(flipped.cols() == 3);
    CHECK(std::ranges::equal(flipped, std::array<pbqp::Cost, 3>{3, 6, 9}));

    std::array<pbqp::Cost, 9> cols_cells = cells;
    pbqp::MatrixSpan cols(cols_cells.data(), 3, 3);
    cols.erase_cols(every_other);
    CHECK(cols.rows() == 3);
    CHECK(cols.cols() == 1);
    CHECK(std::ranges::equal(cols, std::array<pbqp::Cost, 3>{2, 5, 8}));

    // An empty set leaves the matrix untouched.
    std::array<pbqp::Cost, 9> unchanged_cells = cells;
    pbqp::MatrixSpan unchanged(unchanged_cells.data(), 3, 3);
    unchanged.erase_rows({});
    unchanged.erase_cols({});
    CHECK(std::ranges::equal(unchanged, cells));
}

TEST_CASE("edge choice trades off node costs") {
    // The cheap option of `a` is expensive once the edge is paid for.
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({0, 1});
    const pbqp::NodeId b = graph.add({1, 0});
    graph.link(a, b,
               pbqp::Matrix(2, 2,
                            {
                                10,
                                10, // a=0
                                0,
                                0, // a=1
                            }));

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.choices[a] == 1);
    CHECK(solution.choices[b] == 1);
    CHECK(solution.cost == 1);
}

TEST_CASE("triangle with conflicting colours") {
    // Two colours, an odd cycle, and a penalty for monochromatic edges: one
    // edge has to be paid for no matter what.
    pbqp::Graph graph;
    fe::Vector<pbqp::NodeId, 4> nodes;
    for (int i = 0; i < 3; ++i)
        nodes.emplace_back(graph.add({0, 0}));

    const pbqp::Matrix same_colour(2, 2, {5, 0, 0, 5});
    for (size_t i = 0; i < nodes.size(); ++i)
        graph.link(nodes[i], nodes[(i + 1) % nodes.size()], same_colour);

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.cost == 5);
    CHECK(solution.cost == pbqp::evaluate(graph, solution.choices));
}

// Random tree: RI/RII are exact there, so the heuristic must be optimal.
void check_random_trees(int min_cost, int max_cost, int forbidden_percent) {
    std::mt19937_64 random{7};
    std::uniform_int_distribution<int> cost{min_cost, max_cost};
    std::uniform_int_distribution<int> percent{0, 99};
    std::uniform_int_distribution<size_t> num_options{1, 4};
    const auto next_cost = [&] {
        return percent(random) < forbidden_percent ? pbqp::Cost::inf() : static_cast<pbqp::Cost>(cost(random));
    };

    for (int trial = 0; trial < 200; ++trial) {
        const size_t num_nodes = 2 + static_cast<size_t>(trial % 7);

        pbqp::Graph graph;
        for (size_t i = 0; i < num_nodes; ++i) {
            fe::Vector<pbqp::Cost, 4> values(num_options(random));
            std::ranges::generate(values, next_cost);
            graph.add(values);
        }

        for (pbqp::NodeId n = 1; n < num_nodes; ++n) {
            const pbqp::NodeId m = std::uniform_int_distribution<size_t>{0, n - 1}(random);
            pbqp::Matrix matrix(graph.num_options(m), graph.num_options(n));
            std::ranges::generate(matrix.span(), next_cost);
            if (cost(random) % 2 == 0)
                graph.link(m, n, matrix);
            else
                graph.link(n, m, pbqp::MatrixView(matrix).transposed());
        }

        const pbqp::Solution heuristic = pbqp::solve(graph);
        const pbqp::Solution optimal   = *pbqp::solve_exhaustive(graph);
        CHECK(heuristic.cost == optimal.cost);
        CHECK(heuristic.cost == pbqp::evaluate(graph, heuristic.choices));
    }
}

TEST_CASE("heuristic is optimal on random trees") {
    for (const auto& [min_cost, max_cost, forbidden_percent] :
         {std::tuple{0, 10, 0}, std::tuple{-1000, 1000, 0}, std::tuple{-1000, 1000, 20},
          std::tuple{-1'000'000, 1'000'000, 20}}) {
        CAPTURE(min_cost);
        CAPTURE(max_cost);
        CAPTURE(forbidden_percent);
        check_random_trees(min_cost, max_cost, forbidden_percent);
    }
}

TEST_CASE("long path with large costs stays exact") {
    pbqp::Graph graph;
    pbqp::NodeId previous = graph.add({4000, 4000});
    for (int i = 0; i < 11; ++i) {
        const pbqp::NodeId next = graph.add({4000, 4000});
        graph.link(previous, next, pbqp::Matrix(2, 2, {0, 50, 50, 0}));
        previous = next;
    }
    const pbqp::NodeId last = graph.add({100, 0});
    graph.link(previous, last, pbqp::Matrix(2, 2, {0, 50, 50, 0}));

    CHECK(pbqp::solve(graph).cost == pbqp::solve_exhaustive(graph)->cost);
}

TEST_CASE("rn charges its edges to the neighbours") {
    // Dense instance found by search: without charging the RN rows to the neighbours the result is 51.
    pbqp::Graph graph;
    graph.add({8, 1, 9});
    graph.add({0, 4, 1});
    graph.add({0, 6, 6});
    graph.add({0, 9, 2});
    graph.add({7, 0, 4});
    graph.link(0, 1, pbqp::Matrix(3, 3, {8, 3, 9, 9, 4, 8, 6, 0, 3}));
    graph.link(0, 2, pbqp::Matrix(3, 3, {9, 1, 2, 9, 5, 0, 4, 3, 9}));
    graph.link(0, 3, pbqp::Matrix(3, 3, {4, 3, 4, 7, 7, 6, 0, 7, 9}));
    graph.link(0, 4, pbqp::Matrix(3, 3, {5, 6, 0, 3, 3, 8, 8, 4, 6}));
    graph.link(1, 2, pbqp::Matrix(3, 3, {9, 4, 6, 9, 1, 4, 4, 1, 6}));
    graph.link(1, 3, pbqp::Matrix(3, 3, {6, 2, 8, 2, 1, 8, 7, 2, 7}));
    graph.link(1, 4, pbqp::Matrix(3, 3, {2, 4, 0, 1, 1, 4, 0, 1, 3}));
    graph.link(2, 3, pbqp::Matrix(3, 3, {4, 1, 4, 1, 8, 5, 7, 5, 5}));
    graph.link(2, 4, pbqp::Matrix(3, 3, {9, 5, 4, 4, 5, 2, 2, 1, 0}));
    graph.link(3, 4, pbqp::Matrix(3, 3, {6, 9, 6, 5, 1, 9, 2, 6, 9}));

    CHECK(pbqp::solve(graph).cost == 45);
}

TEST_CASE("heuristic result is always valid") {
    // On general graphs the heuristic may lose, but it must return a valid
    // assignment whose reported cost matches a fresh evaluation.
    std::mt19937_64 random{11};
    std::uniform_int_distribution<int> cost{0, 10};
    const auto next_cost = [&] { return static_cast<pbqp::Cost>(cost(random)); };

    for (int trial = 0; trial < 40; ++trial) {
        const size_t num_nodes = 3 + static_cast<size_t>(trial % 5);
        const size_t options   = 2 + static_cast<size_t>(trial % 2);

        pbqp::Graph graph;
        fe::Vector<pbqp::Cost, 4> values(options);
        for (size_t i = 0; i < num_nodes; ++i) {
            std::ranges::generate(values, next_cost);
            graph.add(values);
        }

        pbqp::Matrix matrix(options, options);
        for (size_t m = 0; m < num_nodes; ++m) {
            for (size_t n = m + 1; n < num_nodes; ++n) {
                if (cost(random) < 5) continue; // roughly half the pairs
                std::ranges::generate(matrix.span(), next_cost);
                graph.link(m, n, matrix);
            }
        }

        const pbqp::Solution heuristic = pbqp::solve(graph);
        const pbqp::Solution optimal   = *pbqp::solve_exhaustive(graph);

        CHECK(heuristic.choices.size() == num_nodes);
        CHECK(heuristic.cost == pbqp::evaluate(graph, heuristic.choices));
        CHECK(heuristic.cost >= optimal.cost);
    }
}

TEST_CASE("negative costs reward coalescing") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({0, 0});
    const pbqp::NodeId b = graph.add({0, 0});
    graph.link(a, b, pbqp::Matrix(2, 2, {-5, 0, 0, -5}));

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.cost == -5);
    CHECK(solution.choices[a] == solution.choices[b]);
}

TEST_CASE("infinity forbids same option") {
    constexpr auto infinity = pbqp::Cost::inf();

    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({0, 0});
    const pbqp::NodeId b = graph.add({0, 0});
    graph.link(a, b, pbqp::Matrix(2, 2, {infinity, 0, 0, infinity}));

    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(solution.choices[a] != solution.choices[b]);
    CHECK(solution.cost == 0);
    CHECK(solution.cost == pbqp::evaluate(graph, solution.choices));
}

TEST_CASE("infinite sums do not wrap") {
    constexpr auto infinity = pbqp::Cost::inf();

    // Three nodes and two colours make one monochromatic edge unavoidable, so the reduction sums several infinities.
    pbqp::Graph graph;
    fe::Vector<pbqp::NodeId, 4> nodes;
    for (int i = 0; i < 3; ++i)
        nodes.emplace_back(graph.add({0, 0}));

    const pbqp::Matrix interference(2, 2, {infinity, 0, 0, infinity});
    for (size_t i = 0; i < nodes.size(); ++i)
        graph.link(nodes[i], nodes[(i + 1) % nodes.size()], interference);

    const pbqp::Solution heuristic = pbqp::solve(graph);
    const pbqp::Solution optimal   = *pbqp::solve_exhaustive(graph);
    CHECK(heuristic.cost == optimal.cost);
    CHECK(heuristic.cost == pbqp::Cost::inf());
    CHECK(heuristic.cost == pbqp::evaluate(graph, heuristic.choices));
}

// Complete graph whose nodes all keep one option after pruning, except for `free_nodes`, which then form a tree.
void pruned_complete_graph(pbqp::Graph& graph, std::mt19937_64& random, size_t num_nodes, size_t free_nodes) {
    std::uniform_int_distribution<int> cost{0, 10};
    std::uniform_int_distribution<size_t> num_options{2, 4};
    const auto next_cost = [&] { return static_cast<pbqp::Cost>(cost(random)); };

    for (size_t i = 0; i < num_nodes; ++i) {
        pbqp::Costs values(num_options(random));
        std::ranges::generate(values, next_cost);
        if (i >= free_nodes) {
            const size_t kept = std::uniform_int_distribution<size_t>{0, values.size() - 1}(random);
            for (size_t x = 0; x < values.size(); ++x)
                if (x != kept) values[x] = pbqp::Cost::inf();
        }
        graph.add(values);
    }
    for (pbqp::NodeId m = 0; m < num_nodes; ++m) {
        for (pbqp::NodeId n = m + 1; n < num_nodes; ++n) {
            pbqp::Matrix matrix(graph.num_options(m), graph.num_options(n));
            std::ranges::generate(matrix.span(), next_cost);
            graph.link(m, n, matrix);
        }
    }
}

TEST_CASE("pruning makes dense graphs exact") {
    std::mt19937_64 random{53};
    for (int trial = 0; trial < 100; ++trial) {
        pbqp::Graph graph;
        pruned_complete_graph(graph, random, 6, 2);
        const pbqp::Solution heuristic = pbqp::solve(graph);
        CHECK(heuristic.cost == pbqp::solve_exhaustive(graph)->cost);
        CHECK(heuristic.cost == pbqp::evaluate(graph, heuristic.choices));
    }
}

TEST_CASE("heuristic handles forbidden options on dense graphs") {
    std::mt19937_64 random{59};
    std::uniform_int_distribution<int> cost{0, 10};
    std::uniform_int_distribution<int> percent{0, 99};
    std::uniform_int_distribution<size_t> num_options{1, 4};
    const auto next_cost
        = [&] { return percent(random) < 25 ? pbqp::Cost::inf() : static_cast<pbqp::Cost>(cost(random)); };

    for (int trial = 0; trial < 200; ++trial) {
        const size_t num_nodes = 4 + static_cast<size_t>(trial % 4);
        pbqp::Graph graph;
        for (size_t i = 0; i < num_nodes; ++i) {
            pbqp::Costs values(num_options(random));
            std::ranges::generate(values, next_cost);
            graph.add(values);
        }
        for (pbqp::NodeId m = 0; m < num_nodes; ++m) {
            for (pbqp::NodeId n = m + 1; n < num_nodes; ++n) {
                if (percent(random) < 30) continue;
                pbqp::Matrix matrix(graph.num_options(m), graph.num_options(n));
                std::ranges::generate(matrix.span(), next_cost);
                graph.link(m, n, matrix);
            }
        }

        const pbqp::Solution heuristic = pbqp::solve(graph);
        CHECK(heuristic.cost == pbqp::evaluate(graph, heuristic.choices));
        CHECK(heuristic.cost >= pbqp::solve_exhaustive(graph)->cost);
    }
}

struct IntervalInstance {
    pbqp::Graph graph;
    fe::Vector<size_t, 8> peo_position;
};

// Interval graphs are chordal, so k = maximum overlap registers always suffice; affinities add coalescing costs.
void random_interval_instance(IntervalInstance& instance, std::mt19937_64& random, size_t num_nodes) {
    std::uniform_int_distribution<int> start{0, 40};
    std::uniform_int_distribution<int> length{1, 12};
    std::uniform_int_distribution<int> weight{1, 100};
    std::uniform_int_distribution<int> percent{0, 99};

    fe::Vector<std::pair<int, int>, 8> intervals;
    for (size_t i = 0; i < num_nodes; ++i) {
        const int begin = start(random);
        intervals.emplace_back(begin, begin + length(random));
    }
    const auto interferes = [&](size_t a, size_t b) {
        return intervals[a].first < intervals[b].second && intervals[b].first < intervals[a].second;
    };

    size_t registers = 1;
    for (const auto& [begin, end] : intervals) {
        const auto live = std::ranges::count_if(
            intervals, [&](const auto& other) { return other.first <= begin && begin < other.second; });
        registers = std::max(registers, static_cast<size_t>(live));
    }

    for (size_t i = 0; i < num_nodes; ++i)
        instance.graph.add(pbqp::Costs(registers, 0));
    for (pbqp::NodeId m = 0; m < num_nodes; ++m) {
        for (pbqp::NodeId n = m + 1; n < num_nodes; ++n) {
            const bool affinity = percent(random) < 15;
            if (!interferes(m, n) && !affinity) continue;
            const pbqp::Cost copy = affinity ? static_cast<pbqp::Cost>(weight(random)) : 0;
            pbqp::Matrix matrix(registers, registers, copy);
            for (size_t r = 0; r < registers; ++r)
                matrix.span()[r, r] = interferes(m, n) ? pbqp::Cost::inf() : 0;
            instance.graph.link(m, n, matrix);
        }
    }

    // The interval that ends first is simplicial among the remaining ones.
    fe::Vector<size_t, 8> order(num_nodes);
    std::ranges::iota(order, size_t{0});
    std::ranges::sort(order, [&](size_t a, size_t b) { return intervals[a].second < intervals[b].second; });
    instance.peo_position.resize(num_nodes);
    for (size_t i = 0; i < num_nodes; ++i)
        instance.peo_position[order[i]] = i;
}

TEST_CASE("peo priority always colors chordal graphs") {
    std::mt19937_64 random{61};
    for (int trial = 0; trial < 500; ++trial) {
        IntervalInstance instance;
        random_interval_instance(instance, random, 6 + static_cast<size_t>(trial % 30));
        const pbqp::Solution solution = pbqp::solve(instance.graph, instance.peo_position);
        CHECK(solution.feasible());
        CHECK(solution.cost == pbqp::evaluate(instance.graph, solution.choices));
    }
}

TEST_CASE("statistics count every node") {
    std::mt19937_64 random{67};
    IntervalInstance instance;
    random_interval_instance(instance, random, 30);
    const pbqp::Statistics stats = pbqp::solve(instance.graph, instance.peo_position).statistics;
    CHECK(stats.r0 + stats.r1 + stats.r2 + stats.rn + stats.single_option == instance.graph.num_nodes());

    pbqp::Graph path;
    for (int i = 0; i < 5; ++i)
        path.add({1, 2});
    for (pbqp::NodeId n = 1; n < 5; ++n)
        path.link(n - 1, n, pbqp::Matrix(2, 2, {0, 3, 3, 0}));
    const pbqp::Statistics path_stats = pbqp::solve(path).statistics;
    CHECK(path_stats.rn == 0);
    CHECK(path_stats.r0 + path_stats.r1 + path_stats.r2 == 5);
}

TEST_CASE("infeasible instances are reported") {
    pbqp::Graph graph;
    for (int i = 0; i < 3; ++i)
        graph.add({0, 0});
    const pbqp::Matrix interference(2, 2, {pbqp::Cost::inf(), 0, 0, pbqp::Cost::inf()});
    for (pbqp::NodeId n = 0; n < 3; ++n)
        graph.link(n, (n + 1) % 3, interference);
    const pbqp::Solution solution = pbqp::solve(graph);
    CHECK(!solution.feasible());
    CHECK(solution.conflicts.count() >= 2);
    CHECK(solution.conflicts == pbqp::conflicts(graph, solution.choices));

    const auto optimal = pbqp::solve_exhaustive(graph);
    REQUIRE(optimal.has_value());
    CHECK(!optimal->feasible());
    CHECK(optimal->conflicts.count() >= 2);
}

TEST_CASE("conflicts name the forbidden nodes") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({0, 0});
    const pbqp::NodeId b = graph.add({0, pbqp::Cost::inf()});
    const pbqp::NodeId c = graph.add({0, 0});
    const pbqp::NodeId d = graph.add({0, 0});
    graph.link(c, d, pbqp::Matrix(2, 2, {pbqp::Cost::inf(), 0, 0, 0}));

    const pbqp::Choices choices = {0, 1, 0, 0};
    const fe::Bitset expected   = {b, c, d};
    CHECK(pbqp::conflicts(graph, choices) == expected);
    CHECK(!pbqp::conflicts(graph, choices).test(a));
    CHECK(pbqp::conflicts(graph, pbqp::Choices{0, 0, 1, 0}).count() == 0);
    CHECK(pbqp::solve(graph).conflicts.count() == 0);
}

TEST_CASE("add_edge_costs accumulates in either direction") {
    pbqp::Graph graph;
    const pbqp::NodeId m = graph.add({0, 0});
    const pbqp::NodeId n = graph.add({0, 0, 0});

    const pbqp::EdgeId e = graph.add_edge_costs(m, n, pbqp::Matrix(2, 3, {1, 2, 3, 4, 5, 6}));
    CHECK(graph.edge(m, n) == e);
    CHECK(graph.add_edge_costs(m, n, pbqp::Matrix(2, 3, {10, 0, 0, 0, 0, pbqp::Cost::inf()})) == e);
    CHECK(graph.add_edge_costs(n, m, pbqp::Matrix(3, 2, {0, 100, -2, 0, 0, 0})) == e);
    CHECK(graph.num_edges() == 1);
    CHECK(pbqp::Matrix(graph.edge_matrix(e)) == pbqp::Matrix(2, 3, {11, 0, 3, 104, 5, pbqp::Cost::inf()}));
    CHECK(graph.edge_matrix(e, n)[0, 1] == 104);

    graph.add_edge_costs(m, n, pbqp::Matrix(2, 3, {pbqp::Cost::inf().value() - 1, -5, 0, 0, 0, -1}));
    CHECK(graph.edge_matrix(e)[0, 0] == pbqp::Cost::inf());
    CHECK(graph.edge_matrix(e)[0, 1] == -5);
    CHECK(graph.edge_matrix(e)[1, 2] == pbqp::Cost::inf());
}

TEST_CASE("rm merges implied neighbour") {
    // Figure 2 of Buchwald et al. 2011: coloring U implies V, and merging V into U leaves only exact reductions.
    constexpr auto inf = pbqp::Cost::inf();
    pbqp::Graph graph;
    const pbqp::NodeId u = graph.add({0, 0});
    const pbqp::NodeId v = graph.add({0, 0});
    const pbqp::NodeId w = graph.add({0, 0, 0});
    const pbqp::NodeId y = graph.add({0, 0, 0});
    const pbqp::NodeId a = graph.add({0, 0, 0});
    const pbqp::NodeId b = graph.add({0, 0, 0});
    const pbqp::Matrix interference23(2, 3, {inf, 0, 0, 0, inf, 0});
    const pbqp::Matrix interference33(3, 3, {inf, 0, 0, 0, inf, 0, 0, 0, inf});
    const pbqp::Matrix affinity23(2, 3, {0, 1, 1, 1, 0, 1});
    graph.link(u, w, interference23);
    graph.link(u, v, pbqp::Matrix(2, 2, {inf, 0, 0, inf}));
    graph.link(v, w, interference23);
    graph.link(v, y, affinity23);
    graph.link(u, a, affinity23);
    graph.link(w, b, interference33);
    graph.link(a, y, interference33);
    graph.link(y, b, interference33);
    graph.link(a, b, interference33);

    const std::array<size_t, 6> priority = {5, 0, 4, 1, 2, 3};
    const pbqp::Solution solution        = pbqp::solve(graph, priority);
    CHECK(solution.statistics.rm >= 1);
    CHECK(solution.cost == pbqp::solve_exhaustive(graph)->cost);
}

TEST_CASE("evaluate prices an assignment") {
    pbqp::Graph graph;
    static_cast<void>(graph.add({1, 2}));

    CHECK(pbqp::evaluate(graph, pbqp::Choices{1}) == 2);
}

TEST_CASE("exhaustive refuses large instances") {
    pbqp::Graph graph;
    for (int i = 0; i < 4; ++i)
        graph.add({0, 1, 2});
    CHECK(!pbqp::solve_exhaustive(graph, 80).has_value());
    CHECK(pbqp::solve_exhaustive(graph, 81).has_value());
}

TEST_CASE("find edge works in both directions") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({1, 2});
    const pbqp::NodeId b = graph.add({1, 2});

    graph.link(a, b, pbqp::Matrix(2, 2));
    CHECK(graph.edge(a, b) != pbqp::Graph::npos);
    CHECK(graph.edge(b, a) != pbqp::Graph::npos);
}

TEST_CASE("accessors and setters round trip") {
    pbqp::Graph graph;
    const pbqp::NodeId m = graph.add({1, 2});
    const pbqp::NodeId n = graph.add({3, 4, 5});
    const pbqp::EdgeId e = graph.link(m, n, pbqp::Matrix(2, 3, {0, 1, 2, 3, 4, 5}));

    const auto [src, dst] = graph.edge(e);
    CHECK(src == m);
    CHECK(dst == n);
    CHECK(graph.incident_edges(n).size() == 1);
    CHECK(graph.edge_matrix(e)[1, 2] == 5);
    CHECK(graph.edge_matrix(e, n)[2, 1] == 5);

    graph.set_node_costs(m, std::array<pbqp::Cost, 2>{7, 8});
    graph.set_edge_costs(e, pbqp::Matrix(2, 3, {6, 5, 4, 3, 2, 1}));
    CHECK(graph.node_costs(m)[1] == 8);
    CHECK(graph.edge_matrix(e)[1, 2] == 1);
}

TEST_CASE("equal matrices are shared") {
    pbqp::Graph graph;
    fe::Vector<pbqp::NodeId> nodes;
    for (int i = 0; i < 4; ++i)
        nodes.emplace_back(graph.add({0, 0}));
    const pbqp::Matrix interference(2, 2, {pbqp::Cost::inf(), 0, 0, pbqp::Cost::inf()});

    const pbqp::EdgeId ab = graph.link(nodes[0], nodes[1], interference);
    const pbqp::EdgeId bc = graph.link(nodes[1], nodes[2], interference);
    const pbqp::EdgeId cd = graph.link(nodes[2], nodes[3], interference);
    CHECK(graph.num_matrices() == 1);

    graph.add_edge_costs(nodes[0], nodes[1], pbqp::Matrix(2, 2, {0, -1, -1, 0}));
    CHECK(graph.num_matrices() == 2);
    CHECK(graph.edge_matrix(ab)[0, 1] == -1);
    CHECK(graph.edge_matrix(bc)[0, 1] == 0);

    graph.set_edge_costs(bc, graph.edge_matrix(ab));
    graph.set_edge_costs(cd, graph.edge_matrix(ab).transposed());
    CHECK(graph.num_matrices() == 2);

    for (const auto e : {ab, bc, cd})
        graph.set_edge_costs(e, interference);
    CHECK(graph.num_matrices() == 2);
    CHECK(graph.edge_matrix(ab)[0, 1] == 0);
    CHECK(pbqp::solve(graph).cost == 0);
}

TEST_CASE("shared matrices survive random mutations") {
    std::mt19937_64 random{42};
    std::uniform_int_distribution<int> pick{0, 3};
    const std::array<pbqp::Matrix, 4> palette = {
        pbqp::Matrix(2, 2, {0, 1, 2, 3}),
        pbqp::Matrix(2, 2, {pbqp::Cost::inf(), 0, 0, pbqp::Cost::inf()}),
        pbqp::Matrix(2, 2),
        pbqp::Matrix(2, 2, {3, 2, 1, 0}),
    };

    pbqp::Graph graph;
    const size_t num_nodes = 12;
    for (size_t i = 0; i < num_nodes; ++i)
        graph.add({0, 0});

    fe::Vector<pbqp::Matrix> shadow;
    std::uniform_int_distribution<size_t> node{0, num_nodes - 1};
    for (int step = 0; step < 2000; ++step) {
        const pbqp::NodeId m = node(random);
        const pbqp::NodeId n = node(random);
        if (m == n) continue;
        const pbqp::Matrix& costs = palette[static_cast<size_t>(pick(random))];
        const pbqp::EdgeId e      = graph.edge(m, n);
        switch (pick(random)) {
            case 0:
            case 1:
                if (e != pbqp::Graph::npos) {
                    graph.set_edge_costs(e, costs);
                    shadow[e] = costs;
                }
                break;
            default: {
                const pbqp::EdgeId linked = graph.add_edge_costs(m, n, costs);
                if (linked >= shadow.size()) shadow.resize(linked + 1);
                if (e == pbqp::Graph::npos) {
                    shadow[linked] = costs;
                } else {
                    shadow[e]
                        += graph.edge(e).src == m ? pbqp::MatrixView(costs) : pbqp::MatrixView(costs).transposed();
                }
                break;
            }
        }
    }

    fe::Vector<pbqp::Matrix> distinct;
    for (const auto e : graph.edges()) {
        CHECK(pbqp::Matrix(graph.edge_matrix(e)) == shadow[e]);
        if (std::ranges::find(distinct, shadow[e]) == distinct.end()) distinct.emplace_back(shadow[e]);
    }
    CHECK(graph.num_matrices() >= distinct.size());
    CHECK(graph.num_matrices() <= 2000);
}

TEST_CASE("parallel exhaustive matches sequential") {
    std::mt19937_64 random{29};
    std::uniform_int_distribution<int> cost{0, 10};
    const auto next_cost = [&] { return static_cast<pbqp::Cost>(cost(random)); };

    pbqp::Graph graph;
    fe::Vector<pbqp::Cost, 4> values(3);
    for (size_t i = 0; i < 6; ++i) {
        std::ranges::generate(values, next_cost);
        graph.add(values);
    }

    pbqp::Matrix matrix(3, 3);
    for (size_t m = 0; m < 6; ++m) {
        for (size_t n = m + 1; n < 6; ++n) {
            if (cost(random) < 5) continue;
            std::ranges::generate(matrix.span(), next_cost);
            graph.link(m, n, matrix);
        }
    }

    const pbqp::Solution sequential = *pbqp::solve_exhaustive(graph, 1'000'000, 1);
    const pbqp::Solution parallel   = *pbqp::solve_exhaustive(graph, 1'000'000, 4);
    CHECK(sequential.choices == parallel.choices);
    CHECK(sequential.cost == parallel.cost);
}

TEST_CASE("wide option vectors stay exact") {
    std::mt19937_64 random{41};
    std::uniform_int_distribution<int> cost{0, 100};
    const auto next_cost = [&] { return static_cast<pbqp::Cost>(cost(random)); };

    for (const auto options : {size_t{17}, size_t{33}}) {
        CAPTURE(options);
        fe::Vector<pbqp::Cost, 4> values(options);
        pbqp::Matrix matrix(options, options);

        // A single edge exercises RI with a wide row.
        pbqp::Graph pair;
        std::ranges::generate(values, next_cost);
        const pbqp::NodeId a = pair.add(values);
        std::ranges::generate(values, next_cost);
        const pbqp::NodeId b = pair.add(values);
        std::ranges::generate(matrix.span(), next_cost);
        pair.link(a, b, matrix);
        CHECK(pbqp::solve(pair).cost == pbqp::solve_exhaustive(pair)->cost);

        // A triangle has no low-degree node, so RII contracts a wide matrix.
        pbqp::Graph triangle;
        fe::Vector<pbqp::NodeId, 4> nodes;
        for (int i = 0; i < 3; ++i) {
            std::ranges::generate(values, next_cost);
            nodes.emplace_back(triangle.add(values));
        }
        for (size_t i = 0; i < nodes.size(); ++i) {
            std::ranges::generate(matrix.span(), next_cost);
            triangle.link(nodes[i], nodes[(i + 1) % nodes.size()], matrix);
        }
        CHECK(pbqp::solve(triangle).cost == pbqp::solve_exhaustive(triangle)->cost);
    }
}

TEST_CASE("concurrent solves share a graph") {
    std::mt19937_64 random{31};
    std::uniform_int_distribution<int> cost{0, 10};
    const auto next_cost = [&] { return static_cast<pbqp::Cost>(cost(random)); };

    pbqp::Graph graph;
    fe::Vector<pbqp::Cost, 4> values(2);
    for (size_t i = 0; i < 7; ++i) {
        std::ranges::generate(values, next_cost);
        graph.add(values);
    }

    pbqp::Matrix matrix(2, 2);
    for (size_t m = 0; m < 7; ++m) {
        for (size_t n = m + 1; n < 7; ++n) {
            if (cost(random) < 5) continue;
            std::ranges::generate(matrix.span(), next_cost);
            graph.link(m, n, matrix);
        }
    }

    const pbqp::Solution heuristic = pbqp::solve(graph);
    const pbqp::Solution optimal   = *pbqp::solve_exhaustive(graph, 1'000'000, 1);

    constexpr size_t num_threads = 4;
    std::array<pbqp::Solution, num_threads> heuristics;
    std::array<pbqp::Solution, num_threads> optima;
    std::array<std::thread, num_threads> workers;

    for (size_t i = 0; i < num_threads; ++i) {
        workers[i] = std::thread([&, i] {
            heuristics[i] = pbqp::solve(graph);
            optima[i]     = *pbqp::solve_exhaustive(graph, 1'000'000, 2);
        });
    }
    for (auto& worker : workers)
        worker.join();

    for (size_t i = 0; i < num_threads; ++i) {
        CHECK(heuristics[i].cost == heuristic.cost);
        CHECK(optima[i].choices == optimal.choices);
    }
}

TEST_CASE("dot shows costs and choices") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({3, 4});
    const pbqp::NodeId b = graph.add({7});
    graph.link(a, b, pbqp::Matrix(2, 1, {pbqp::Cost::inf(), 5}));

    std::ostringstream plain;
    pbqp::write_dot(plain, graph);
    const std::string text = plain.str();
    CHECK(text.starts_with("graph pbqp {"));
    CHECK(text.find("n0 -- n1") != std::string::npos);
    CHECK(text.find("<td>&#8734;</td><td>5</td>") == std::string::npos);
    CHECK(text.find("<tr><td>&#8734;</td></tr><tr><td>5</td></tr>") != std::string::npos);
    CHECK(text.find("palegreen") == std::string::npos);

    std::ostringstream chosen;
    pbqp::write_dot(chosen, graph, pbqp::solve(graph).choices);
    CHECK(chosen.str().find(R"(<td bgcolor="palegreen">5</td>)") != std::string::npos);
    CHECK(chosen.str().find(R"(<td bgcolor="palegreen">4</td>)") != std::string::npos);
}

void read_json(std::string_view text, pbqp::Instance& instance) {
    std::istringstream in{std::string(text)};
    pbqp::read_json(in, instance);
}

std::string write_json(const pbqp::Graph& graph, fe::View<size_t> rn_priority = {}) {
    std::ostringstream out;
    pbqp::write_json(out, graph, rn_priority);
    return out.str();
}

TEST_CASE("json round trips ids infinity and priorities") {
    pbqp::Graph graph;
    const pbqp::NodeId a = graph.add({-3, pbqp::Cost::inf()});
    const pbqp::NodeId c = graph.add({7, 0, 2});
    graph.link(c, a, pbqp::Matrix(3, 2, {1, pbqp::Cost::inf(), -5, 0, 4, 2}));
    const std::array<size_t, 2> priority{5, 9};

    const std::string text = write_json(graph, priority);
    pbqp::Instance instance;
    read_json(text, instance);
    CHECK(text.find(R"("inf")") != std::string::npos);
    CHECK(text.find(R"("rn_priority": [5, 9])") != std::string::npos);
    CHECK(instance.graph.num_nodes() == 2);
    CHECK(std::ranges::equal(instance.graph.node_costs(a), graph.node_costs(a)));
    CHECK(std::ranges::equal(instance.graph.node_costs(c), graph.node_costs(c)));
    const pbqp::EdgeId e = instance.graph.edge(a, c);
    REQUIRE(e != pbqp::Graph::npos);
    CHECK(instance.graph.edge(e).src == c);
    CHECK(std::ranges::equal(instance.graph.edge_matrix(e), graph.edge_matrix(graph.edge(a, c))));
    REQUIRE(instance.rn_priority.size() == 2);
    CHECK(instance.rn_priority[c] > instance.rn_priority[a]);
    CHECK(write_json(instance.graph, instance.rn_priority) == text);
    pbqp::Instance plain;
    read_json(write_json(graph), plain);
    CHECK(plain.rn_priority.empty());
}

TEST_CASE("json skips unknown keys and optional fields") {
    pbqp::Instance instance;
    read_json(R"({
        "edges": [{"costs": ["inf", 0, 0, "inf"], "dst": 0, "src": 1, "weight": 1.5}],
        "meta": {"comment": "ignored", "nested": [true, null, -1.5e3]},
        "nodes": [{"costs": [0, "inf"], "name": "a"}, {"costs": [3, 2]}]
    })",
              instance);
    const pbqp::Graph& graph = instance.graph;
    CHECK(graph.num_nodes() == 2);
    const pbqp::EdgeId e = graph.edge(0, 1);
    REQUIRE(e != pbqp::Graph::npos);
    CHECK(graph.edge(e).src == 1);
    CHECK(pbqp::Matrix(graph.edge_matrix(e)) == pbqp::Matrix(2, 2, {pbqp::Cost::inf(), 0, 0, pbqp::Cost::inf()}));
    CHECK(instance.rn_priority.empty());
    CHECK(pbqp::solve(graph).cost == 2);
}

TEST_CASE("json reports malformed input") {
    const auto error = [](std::string_view text) -> std::string {
        try {
            pbqp::Instance instance;
            read_json(text, instance);
        } catch (const std::exception& e) { return e.what(); }
        return {};
    };
    const auto rejects = [&](std::string_view text, std::string_view message) {
        CAPTURE(text);
        CAPTURE(message);
        CHECK(error(text).find(message) != std::string::npos);
    };

    rejects(R"({"nodes": [{"costs": [1]}]} #)", "invalid input character");
    rejects(R"({"nodes": [{"costs": [1, 2.5]}]})", "expected cost, got");
    rejects(R"json({"nodes": [{"costs": ["1]})json", "unterminated string");
    rejects(R"({"nodes": [{"costs": ["1"]}]})", "expected a cost, got");
    rejects(R"({"nodes": [{"costs": [2147483647]}]})", "out of range");
    rejects(R"({"nodes": [{"costs": []}]})", "no options");
    rejects(R"({"nodes": [{"cost": [0]}]})", "node without");
    rejects(R"({"nodes": [{"costs": [true, 2147483648]}]})", "2 error(s)");
    rejects(R"({"nodes": [{"costs": [0]}], "edges": [{"src": 0, "dst": 1, "costs": [0]}]})", "unknown node 1");
    rejects(R"({"nodes": [{"costs": [0]}, {"costs": [0]}], "edges": [{"src": 0, "target": 1, "costs": [0]}]})",
            "edge without `dst`");
    rejects(R"({"nodes": [{"costs": [0]}, {"costs": [0, 0]}], "edges": [{"src": 0, "dst": 1, "costs": [0]}]})",
            "expected 1 × 2 costs, got 1");
    rejects(R"({"nodes": [{"costs": [0]}, {"costs": [0, 0]}],
                "edges": [{"src": 0, "dst": 1, "cols": 3, "costs": [0, 0]}]})",
            "`cols` is 3");
    rejects(R"({"nodes": [{"costs": [0]}, {"costs": [0]}], "rn_priority": [0, 1, 0]})", "expected 2 priorities, got 3");
}

TEST_CASE("libFirm instances are solved optimally") {
    struct Case {
        const char* file;
        size_t nodes;
        size_t edges;
    };
    for (const auto& [file, nodes, edges] : {
             Case{"normalLibfirmGraph.json", 42, 31},
             Case{"smallLibfirmGraph.json", 42, 31},
             Case{"problematic.json", 45, 69},
         }) {
        CAPTURE(file);
        std::ifstream in{std::string(PBQP_TEST_DATA) + "/" + file};
        pbqp::Instance instance;
        try {
            pbqp::read_json(in, instance, file);
        } catch (const std::exception& error) {
            FAIL_CHECK(error.what());
            continue;
        }
        const pbqp::Graph& graph = instance.graph;
        CHECK(graph.num_nodes() == nodes);
        CHECK(graph.num_edges() == edges);
        CHECK(std::ranges::all_of(graph.nodes(), [&](pbqp::NodeId n) { return instance.rn_priority[n] != 0; }));

        // Every cost in these instances is non-negative, so 0 is optimal.
        const pbqp::Solution solution = pbqp::solve(graph, instance.rn_priority);
        CHECK(solution.cost == 0);
        CHECK(pbqp::evaluate(graph, solution.choices) == 0);

        const std::string text = write_json(graph, instance.rn_priority);
        pbqp::Instance back;
        read_json(text, back);
        CHECK(write_json(back.graph, back.rn_priority) == text);
    }
}

} // namespace
