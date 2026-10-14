#pragma once

#include <mim/def.h>

namespace mim::plug::tensor::phase {

/// Checks statically decidable gather constraints, blaming @p app for a violation. Returns false for unresolved
/// relations.
bool check_gather_shape_constraints(const App* app,
                                    const Def* rank,
                                    const Def* dim,
                                    const Def* src_shape,
                                    const Def* idx_shape);

/// Checks statically decidable scatter constraints, blaming @p app for a violation. Returns false for unresolved
/// relations.
bool check_scatter_shape_constraints(const App* app,
                                     const Def* rank,
                                     const Def* dim,
                                     const Def* src_shape,
                                     const Def* idx_shape,
                                     const Def* updates_shape);

} // namespace mim::plug::tensor::phase
