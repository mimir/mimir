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

/// Read the pbqp-papa JSON format, version 1 (costs as strings) or 2 (costs as integers, `"inf"`).
/// The `index` of a node becomes its id; unused ids are removed nodes.
/// Throws `fe::Error::Bail`, a `std::exception`, on malformed input; `path` only names the input in its message.
[[nodiscard]] Instance read_json(std::istream& in, const std::filesystem::path& path = "<input>");

/// Write version 2 of the pbqp-papa JSON format.
/// Requires `rn_priority` to be empty or to cover every node id.
void write_json(std::ostream& out, const Graph& graph, fe::View<size_t> rn_priority = {});

} // namespace pbqp
