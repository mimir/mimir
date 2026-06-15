#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/vector.h>

#include <cstdint>

#include <fe/sym.h>

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/world.h>

#include <mim/pass/optimize.h>

namespace nb = nanobind;

namespace mim {

using DefVector = std::vector<const Def*>;

void init_world(nb::module_& m) {
    // clang-format off
    nb::class_<World>(m, "World", nb::never_destruct())
        .def("write", nb::overload_cast<>(&World::write))
        .def("top_nat", &World::top_nat, nb::rv_policy::reference_internal)
        .def("type_bool", &World::type_bool, nb::rv_policy::reference_internal)
        .def("type_i8", &World::type_i8, nb::rv_policy::reference_internal)
        .def("lit_nat_0", &World::lit_nat_0, nb::rv_policy::reference_internal)
        .def("lit_bool", &World::lit_bool, nb::rv_policy::reference_internal)
        .def("lit_ff", &World::lit_ff, nb::rv_policy::reference_internal)
        .def("lit_tt", &World::lit_tt, nb::rv_policy::reference_internal)
        .def("lit", &World::lit, nb::rv_policy::reference_internal)
        .def("type_idx", static_cast<const Def* (World::*)(const Def*)>(&World::type_idx), nb::rv_policy::reference_internal)
        .def("type_idx", static_cast<const Def* (World::*)(nat_t)>(&World::type_idx), nb::rv_policy::reference_internal)
        .def("cn", [](World& w, DefVector doms) { return w.cn(Defs(doms)); }, nb::rv_policy::reference_internal)
        .def("lit_i8", static_cast<const Lit* (World::*)(u8)>(&World::lit_i8), nb::rv_policy::reference_internal)
        .def("type_i32", &World::type_i32, nb::rv_policy::reference_internal)
        .def("sym", static_cast<fe::Sym (World::*)(std::string_view)>(&World::sym))
        .def("pi",      [](World& w, const Def* dom , const Def* codom ) { return w.pi     (      dom,       codom  ); }, nb::rv_policy::reference_internal)
        .def("pi",      [](World& w, const Def* dom , DefVector  codoms) { return w.pi     (      dom,  Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("pi",      [](World& w, DefVector  doms, const Def* codom ) { return w.pi     (Defs(doms),      codom  ); }, nb::rv_policy::reference_internal)
        .def("pi",      [](World& w, DefVector  doms, DefVector  codoms) { return w.pi     (Defs(doms), Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("mut_lam", [](World& w, const Def* dom , const Def* codom ) { return w.mut_lam(      dom,       codom  ); }, nb::rv_policy::reference_internal)
        .def("mut_lam", [](World& w, const Def* dom , DefVector  codoms) { return w.mut_lam(      dom,  Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("mut_lam", [](World& w, DefVector  doms, const Def* codom ) { return w.mut_lam(Defs(doms),      codom  ); }, nb::rv_policy::reference_internal)
        .def("mut_lam", [](World& w, DefVector  doms, DefVector  codoms) { return w.mut_lam(Defs(doms), Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("mut_fun", [](World& w, const Def* dom , const Def* codom ) { return w.mut_fun(      dom,       codom  ); }, nb::rv_policy::reference_internal)
        .def("mut_fun", [](World& w, const Def* dom , DefVector  codoms) { return w.mut_fun(      dom,  Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("mut_fun", [](World& w, DefVector  doms, const Def* codom ) { return w.mut_fun(Defs(doms),      codom  ); }, nb::rv_policy::reference_internal)
        .def("mut_fun", [](World& w, DefVector  doms, DefVector  codoms) { return w.mut_fun(Defs(doms), Defs(codoms)); }, nb::rv_policy::reference_internal)
        .def("mut_con", [](World& w, const Def* dom                    ) { return w.mut_con(     dom                ); }, nb::rv_policy::reference_internal)
        .def("mut_con", [](World& w, DefVector  doms                   ) { return w.mut_con(Defs(doms)              ); }, nb::rv_policy::reference_internal)
        .def(         "app", [](World& w, const Def* callee, const Def* arg ) { return w.         app(callee,      arg ) ; }, nb::rv_policy::reference_internal)
        .def(         "app", [](World& w, const Def* callee, DefVector  args) { return w.         app(callee, Defs(args)); }, nb::rv_policy::reference_internal)
        .def("implicit_app", [](World& w, const Def* callee, const Def* arg ) { return w.implicit_app(callee,      arg ) ; }, nb::rv_policy::reference_internal)
        .def("implicit_app", [](World& w, const Def* callee, DefVector  args) { return w.implicit_app(callee, Defs(args)); }, nb::rv_policy::reference_internal)
        .def("arr",     [](World& w, Def* arity, Def* body) { return w.arr(arity, body); }, nb::rv_policy::reference_internal)
        .def("optimize", [](World& w) { optimize(w); })
        .def("set", [](World& w, std::string name) { w.set(name); })
        .def("dot", static_cast<void (World::*)(const char*, bool, bool) const>(&World::dot))
        .def("annex", [](World& w, uint64_t id) { return w.annex(id); }, nb::rv_policy::reference_internal);
    // clang-format on
}

} // namespace mim
