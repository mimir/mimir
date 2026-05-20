#include <nanobind/nanobind.h>

namespace nb = nanobind;

namespace fe {
void init_sym(nb::module_&);
void init_sym_pool(nb::module_&);
} // namespace fe

namespace mim {
void init_world(nb::module_&);
void init_driver(nb::module_&);
void init_sym(nb::module_&);
void init_flags(nb::module_&);
void init_log(nb::module_&);
void init_def(nb::module_&);
void init_lam(nb::module_&);
void init_tuple(nb::module_&);
void register_error(nb::module_&);
} // namespace mim

namespace mim::ast {
void init_ast(nb::module_&);
void init_parser(nb::module_&);
} // namespace mim::ast

NB_MODULE(_mim, m) {
    // Register foundational types first so World's method signatures resolve
    // to Python class names instead of raw C++ spellings ("mim::Lit" -> "Lit").
    // Please consider The ordering for future added Types.
    fe::init_sym(m);
    fe::init_sym_pool(m);

    mim::init_def(m);
    mim::init_lam(m);
    mim::init_tuple(m);
    mim::init_world(m);
    mim::init_flags(m);
    mim::init_log(m);
    mim::init_driver(m);
    mim::ast::init_ast(m);
    mim::ast::init_parser(m);
    // mim::init_app(m);

    mim::register_error(m);
}
