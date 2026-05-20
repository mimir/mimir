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
    nb::class_<Driver, fe::SymPool>(m, "Driver")
        .def(nb::init<>())
        .def("world", &Driver::world, nb::rv_policy::reference_internal)
        // clang-format off
        .def("add_import", [](Driver& driver, std::string path, std::string str) { return driver.imports().add(fs::path(path), driver.sym(str), ast::Tok::Tag::K_import); }, nb::rv_policy::reference_internal)
        .def("add_plugin", [](Driver& driver, std::string path, std::string str) { return driver.imports().add(fs::path(path), driver.sym(str), ast::Tok::Tag::K_plugin); }, nb::rv_policy::reference_internal)
        // clang-format on
        .def("add_search_path", &Driver::add_search_path)
        .def("log", &Driver::log, nb::rv_policy::reference_internal)
        .def("backend",
             [](Driver& d, std::string backend, std::string output_file_name, World& world) {
                 std::ofstream ofs(output_file_name);
                 d.backend(backend)(world, ofs);
                 ofs.close();
             })
        .def("load_plugins",
             [](Driver& d, std::vector<std::string> plugins) { ast::load_plugins(d.world(), plugins); });
}

} // namespace mim
