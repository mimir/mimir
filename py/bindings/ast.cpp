#include <nanobind/nanobind.h>

#include <mim/ast/ast.h>

namespace nb = nanobind;

namespace mim::ast {

void init_ast(nb::module_& m) { nb::class_<mim::ast::AST>(m, "AST").def(nb::init<>()).def(nb::init<mim::World&>()); }

} // namespace mim::ast
