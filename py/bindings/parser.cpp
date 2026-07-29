#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>

#include <mim/ast/parser.h>

namespace nb = nanobind;

namespace mim::ast {

void init_parser(nb::module_& m) {
    nb::class_<mim::ast::Parser>(m, "Parser", nb::never_destruct())
        .def(nb::init<mim::ast::AST&>())
        .def("plugin", [](mim::ast::Parser& p, const std::string plug) {
            auto& ast = p.ast();
            if (auto mod = p.import(plug, mim::ast::Tok::Tag::K_plugin)) mod->compile(ast);
        });
}

} // namespace mim::ast
