#include "mim/lattice.h"

#include <fe/algo.h>

#include "mim/util/gid.h"

namespace mim {

size_t Bound::find(const Def* type) const {
    auto lt = GIDLt<const Def*>();
    auto i  = isa_mut() ? std::find(ops().begin(), ops().end(), type) : fe::binary_find(ops(), type, lt);
    return i == ops().end() ? size_t(-1) : i - ops().begin();
}

} // namespace mim
