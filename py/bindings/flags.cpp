#include <nanobind/nanobind.h>

#include <mim/flags.h>

namespace nb = nanobind;

namespace mim {

void init_flags(nb::module_& m) {
    nb::class_<Flags>(m, "Flags").def(nb::init<>()).def_rw("scalarize_threshold", &Flags::scalarize_threshold);
}
} // namespace mim
