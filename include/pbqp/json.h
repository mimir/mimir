#pragma once

#include <filesystem>
#include <istream>
#include <ostream>

#include <fe/span.h>
#include <fe/vector.h>

#include "pbqp/graph.h"

namespace pbqp {

/// A graph together with the optional `rn_priority` for `solve`.
struct Instance {
    Graph graph;
    /// Empty, or one entry per node id.
    fe::Vector<size_t> rn_priority;
};

/// Read a PBQP instance into the empty `instance`:
///
///     {
///       "nodes": [{"costs": [0, "inf"]}, {"costs": [7, 0, 2]}],
///       "edges": [{"src": 1, "dst": 0, "rows": 3, "cols": 2, "costs": [1, "inf", -5, 0, 4, 2]}],
///       "rn_priority": [5, 9]
///     }
///
/// The position of a node in `nodes` is its id.
/// Edge `costs` are row-major with `src` as the row; `rows` and `cols` are optional and only checked.
/// `"inf"` is Cost::inf, `rn_priority` is optional, and unknown keys are skipped.
/// Throws `fe::Error::Bail`, a `std::exception`, on malformed input; `path` only names the input in its message.
void read_json(std::istream& in, Instance& instance, const std::filesystem::path& path = "<input>");

/// Write the format that `read_json` reads.
/// Requires `rn_priority` to be empty or to have one entry per node.
void write_json(std::ostream& out, const Graph& graph, fe::View<size_t> rn_priority = {});

} // namespace pbqp
