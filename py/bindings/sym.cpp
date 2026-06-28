#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <fe/sym.h>

#include <mim/def.h>

namespace nb = nanobind;

namespace fe {

void init_sym(nb::module_& m) {
    nb::class_<fe::Sym>(m, "Sym")
        .def(nb::init<>())
        .def("empty", &fe::Sym::empty)
        .def("size", &fe::Sym::size)
        .def("view", &fe::Sym::view)
        .def("str", &fe::Sym::str);
}

void init_sym_pool(nb::module_& m) {
    nb::class_<fe::SymPool>(m, "SymPool")
        .def(nb::init<>())
        .def("sym", static_cast<Sym (SymPool::*)(std::string_view)>(&fe::SymPool::sym))
        .def("sym", static_cast<Sym (SymPool::*)(const std::string&)>(&fe::SymPool::sym))
        .def("sym", static_cast<Sym (SymPool::*)(const char*)>(&fe::SymPool::sym));
}

} // namespace fe
