#pragma once

#include <cstddef>

#include <optional>

#include <fe/bitset.h>
#include <fe/span.h>

#include "pbqp/graph.h"

namespace pbqp {

/// How often `solve` applied each reduction.
struct Statistics {
    size_t r0                = 0;
    size_t r1                = 0;
    size_t r2                = 0;
    size_t rn                = 0;
    size_t rm                = 0; // neighbours merged before an RN decision
    size_t single_option     = 0; // degree >= 3 nodes with one option left, reduced exactly
    size_t forbidden_options = 0;
    size_t dropped_edges     = 0; // independent edges removed by normalization
};

/// A complete assignment of options to nodes.
struct Solution {
    /// choices[n] is the option chosen for node `n`.
    Choices choices;
    /// Total cost of the assignment under the original graph.
    CostSum cost = 0;
    /// Nodes whose choice hits a kInfinity entry, see `conflicts`; empty iff feasible.
    fe::Bitset conflicts;
    Statistics statistics;

    [[nodiscard]] bool feasible() const { return cost != kInfiniteSum; }
};

/// All solver functions are safe to call concurrently on a graph that nobody mutates meanwhile.
///
/// Cost of `choices` under `graph`, or kInfiniteSum if it picks a kInfinity entry.
/// Requires a valid option for every live node.
[[nodiscard]] CostSum evaluate(const Graph& graph, fe::View<OptionId> choices);

/// Nodes whose choice is kInfinity in their cost vector or, given the neighbour's choice, in an incident matrix.
/// These are the candidates to spill or split when an assignment is infeasible.
/// Requires a valid option for every live node.
[[nodiscard]] fe::Bitset conflicts(const Graph& graph, fe::View<OptionId> choices);

/// Heuristic PBQP solver.
///
/// Reduces degree 0, 1 (RI) and 2 (RII) nodes exactly and higher-degree nodes with the RN heuristic.
/// Normalization keeps intermediate costs relative, so they saturate only where option costs differ by kInfinity.
/// Short of that, the result is optimal whenever no RN step is needed, e.g. on trees.
/// RN decides the node with the highest `rn_priority[n]` first; ties go to the highest degree, then the widest cost
/// spread. SSA-based register allocation passes PEO positions, so coloring cannot fail (Buchwald et al. 2011). Requires
/// `rn_priority` to be empty or to cover every node id.
[[nodiscard]] Solution solve(const Graph& graph, fe::View<size_t> rn_priority = {});

/// Brute-force optimum, useful to cross-check small instances.
/// The search space is split across `num_threads` threads (0 = hardware concurrency).
/// Returns nothing if there are more than `max_combinations` candidate assignments.
[[nodiscard]] std::optional<Solution>
solve_exhaustive(const Graph& graph, size_t max_combinations = 10'000'000, unsigned num_threads = 0);

} // namespace pbqp
