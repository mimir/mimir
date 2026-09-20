#include "mim/plug/clos/clos.h"

namespace mim::plug::clos {

template<anno o>
const Def* normalize_clos(const Def*, const Def*, const Def* arg) {
    return o == anno::bottom ? arg : nullptr;
}

MIM_clos_NORMALIZER_IMPL

} // namespace mim::plug::clos
