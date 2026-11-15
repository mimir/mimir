#pragma once

#include <ostream>

#include <fe/span.h>

#include "pbqp/graph.h"

namespace pbqp {

/// Write `graph` in Graphviz DOT format with cost vectors and matrices as HTML-like labels.
/// Edge matrices are titled `src × dst`, i.e. rows × columns.
/// Highlights the entries picked by `choices`, which must be empty or cover every node id.
void write_dot(std::ostream& out, const Graph& graph, fe::View<OptionIndex> choices = {});

} // namespace pbqp
