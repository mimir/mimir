#pragma once

#include <mim/def.h>

namespace mim::plug::tensor::phase {

/// Checks statically decidable gather constraints. Returns false for unresolved relations.
bool check_gather_shape_constraints(const Def* rank,
                                    const Def* dim,
                                    const Def* source_shape,
                                    const Def* index_shape);

/// Checks statically decidable scatter constraints. Returns false for unresolved relations.
bool check_scatter_shape_constraints(const Def* rank,
                                     const Def* dim,
                                     const Def* source_shape,
                                     const Def* index_shape,
                                     const Def* updates_shape);

} // namespace mim::plug::tensor::phase
