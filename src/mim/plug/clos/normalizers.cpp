#include "mim/plug/clos/clos.h"

namespace mim::plug::clos {

template<attr o>
const Def* normalize_clos(const Def*, const Def*, const Def* arg) {
    return o == attr::bottom ? arg : nullptr;
}

MIM_clos_NORMALIZER_IMPL

} // namespace mim::plug::clos
