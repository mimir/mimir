#include <nanobind/nanobind.h>

#include <mim/util/log.h>

namespace nb = nanobind;

namespace mim {

void init_log(nb::module_& m) {
    nb::enum_<Log::Level>(m, "Level")
        .value("Error", Log::Level::Error)
        .value("Warn", Log::Level::Warn)
        .value("Info", Log::Level::Info)
        .value("Verbose", Log::Level::Verbose)
        .value("Debug", Log::Level::Debug)
        .export_values();

    nb::class_<Log>(m, "Log")
        .def(nb::init<Flags&>())
        .def("set", static_cast<Log& (Log::*)(Log::Level)>(&Log::set), nb::rv_policy::reference)
        .def("set_stdout", [](Log& self) -> Log& { return self.set(&std::cout); }, nb::rv_policy::reference);
}

} // namespace mim
