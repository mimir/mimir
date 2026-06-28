#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <mim/def.h>
#include <mim/lam.h>

namespace nb = nanobind;

namespace mim {

void init_lam(nb::module_& m) {
    // clang-format off
    nb::class_<Pi, Def>(m, "Pi", nb::never_destruct())
        .def(  "dom", [](Pi& p) { return p.dom(); }, nb::rv_policy::reference_internal)
        .def("codom", [](Pi& p) { return p.codom(); }, nb::rv_policy::reference_internal);

    nb::class_<Lam, Def>(m, "Lam", nb::never_destruct())
        .def("var", static_cast<const Def* (Lam::*)()>(&Def::var), nb::rv_policy::reference_internal)
        .def("app", [](Lam& l, bool filter, Def* callee, std::vector<Def*> args) { return l.app(filter, callee, Defs(args)); }, nb::rv_policy::reference_internal)
        .def("set", [](Lam& l, std::string s) { return l.set(s); }, nb::rv_policy::reference_internal)
        .def("externalize", &Lam::externalize);

    nb::class_<App, Def>(m, "App", nb::never_destruct())
        .def("callee", &App::callee, nb::rv_policy::reference_internal)
        .def("arg", [](App& a) { return a.arg(); }, nb::rv_policy::reference_internal);
    // clang-format on
}

} // namespace mim
