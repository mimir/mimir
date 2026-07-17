#include <nanobind/nanobind.h>

#include <mim/flags.h>

namespace nb = nanobind;

namespace mim {

void init_flags(nb::module_& m) {
    auto flags = nb::class_<Flags>(m, "Flags")
                     .def(nb::init<>())
                     .def_rw("scalarize_threshold", &Flags::scalarize_threshold);

    nb::enum_<Flags::Profile>(flags, "Profile")
        .value("None_", Flags::Profile::None)
        .value("Summary", Flags::Profile::Summary)
        .value("Tree", Flags::Profile::Tree)
        .value("Trace", Flags::Profile::Trace);
    
    flags.def_rw("profile", &Flags::profile);
}
} // namespace mim
