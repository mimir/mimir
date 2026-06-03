#include <nanobind/nanobind.h>
#include <nanobind/stl/vector.h>

#include <mim/def.h>
#include <mim/tuple.h>

namespace nb = nanobind;

namespace mim {

void init_tuple(nb::module_& m) {
    nb::class_<Prod, Def>(m, "Prod", nb::never_destruct());
    nb::class_<Seq, Def>(m, "Seq", nb::never_destruct());
    nb::class_<Sigma, Prod>(m, "Sigma", nb::never_destruct());
    nb::class_<Tuple, Prod>(m, "Tuple", nb::never_destruct());
    nb::class_<Extract, Def>(m, "Extract", nb::never_destruct());
    nb::class_<Insert, Def>(m, "Insert", nb::never_destruct());
    nb::class_<Arr, Seq>(m, "Arr", nb::never_destruct());
    nb::class_<Pack, Seq>(m, "Pack", nb::never_destruct());
}

} // namespace mim
