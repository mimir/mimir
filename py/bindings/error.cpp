#include <nanobind/nanobind.h>

#include <mim/util/dbg.h>

namespace nb = nanobind;

namespace mim {

void register_error(nb::module_& m) { nb::exception<Error::Bail>(m, "MIM_Error"); }

} // namespace mim
