#include <mim/axm.h>
#include <mim/world.h>

#include "mim/plug/matrix/matrix.h"

namespace mim::plug::matrix {

// The element-level normalizers (read / insert / shape) now live in the `buffer` plugin.
// These matrix normalizers are currently no-ops: the corresponding simplifications are performed by the lowering
// phases instead.
const Def* normalize_map_reduce(const Def*, const Def*, const Def*) { return {}; }
const Def* normalize_prod(const Def*, const Def*, const Def*) { return {}; }
const Def* normalize_transpose(const Def*, const Def*, const Def*) { return {}; }

MIM_matrix_NORMALIZER_IMPL

} // namespace mim::plug::matrix
