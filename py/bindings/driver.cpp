#include <sstream>

#include <nanobind/nanobind.h>
#include <nanobind/stl/filesystem.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <fe/sym.h>

#include <mim/driver.h>

#include <mim/ast/ast.h>

namespace nb = nanobind;

namespace mim {

void init_driver(nb::module_& m) {
    nb::class_<Profiler>(m, "Profiler")
        .def("empty", &Profiler::empty)
        // clang-format off
        .def("summary",      [](const Profiler& p) { std::ostringstream os; p.summary(os);      return os.str(); })
        .def("tree",         [](const Profiler& p) { std::ostringstream os; p.tree(os);         return os.str(); })
        .def("chrome_trace", [](const Profiler& p) { std::ostringstream os; p.chrome_trace(os); return os.str(); });
    // clang-format on

    nb::class_<Driver, fe::SymPool>(m, "Driver")
        .def(nb::init<>())
        .def(nb::init<std::string>())
        .def("world", &Driver::world, nb::rv_policy::reference_internal)
        // clang-format off
        .def("add_import", [](Driver& driver, std::string path, std::string str) { return driver.imports().add(fs::path(path), driver.sym(str), ast::Tok::Tag::K_import); }, nb::rv_policy::reference_internal)
        .def("add_plugin", [](Driver& driver, std::string path, std::string str) { return driver.imports().add(fs::path(path), driver.sym(str), ast::Tok::Tag::K_plugin); }, nb::rv_policy::reference_internal)
        .def("add_search_path", &Driver::add_search_path)
        .def("flags", [](Driver& driver) -> Flags& { return driver.flags(); }, nb::rv_policy::reference_internal)
        .def("profiler", [](Driver& driver) -> Profiler& { return driver.profiler(); }, nb::rv_policy::reference_internal)
        .def("log", &Driver::log, nb::rv_policy::reference_internal)
        .def("load_plugin",  [](Driver& d,             std::string  plugin ) { ast::load_plugin (d.world(), plugin ); })
        .def("load_plugins", [](Driver& d, std::vector<std::string> plugins) { ast::load_plugins(d.world(), plugins); });
    // clang-format on
}

} // namespace mim
