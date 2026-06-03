#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <mim/def.h>
#include <mim/driver.h>
#include <mim/world.h>

namespace nb = nanobind;

namespace mim {

void init_def(nb::module_& m) {
    // clang-format off
    nb::class_<Def>(m, "Def", nb::never_destruct())
        .def("world",  [](Def& d) { return &d.world(); }, nb::rv_policy::reference_internal)
        .def("driver", [](Def& d) { return &d.world().driver(); }, nb::rv_policy::reference_internal)
        .def("var",  nb::overload_cast<            >(&Def::var             ), nb::rv_policy::reference_internal)
        .def("proj", nb::overload_cast<nat_t, nat_t>(&Def::proj, nb::const_), nb::rv_policy::reference_internal)
        .def("proj", nb::overload_cast<       nat_t>(&Def::proj, nb::const_), nb::rv_policy::reference_internal)
        .def("dump", nb::overload_cast<            >(&Def::dump, nb::const_))
        .def("__getitem__", [](const Def& d, nb::object index) -> const Def* {
            if (nb::isinstance<nb::int_ >(index)) return d.proj(nb::cast<nat_t>(index));
            if (nb::isinstance<nb::tuple>(index)) {
                auto tuple = nb::borrow<nb::tuple>(index);
                if (tuple.size() == 2) return d.proj(nb::cast<nat_t>(tuple[0]), nb::cast<nat_t>(tuple[1]));
                throw nb::index_error("tuple index must have exactly 2 elements (arity, index)");
            }
            throw nb::index_error("index must be int or (arity, index)");
        }, nb::rv_policy::reference_internal)
        .def("externalize", &Def::externalize)
        .def("set", static_cast<Def* (Def::*)(std::string)>(&Def::set), nb::rv_policy::reference_internal)
        .def("num_projs", &Def::num_projs)
        // clang-format on
        .def(
            "projs",
            [](Def& d, nat_t a) {
                std::vector<const Def*> ret_vec;
                ret_vec.reserve(a);

                for (auto* proj : d.projs()) {
                    if (ret_vec.size() == a) break;
                    ret_vec.push_back(proj);
                }

                return ret_vec;
            },
            nb::rv_policy::reference_internal);

    nb::class_<Lit, Def>(m, "Lit", nb::never_destruct());
}

} // namespace mim
