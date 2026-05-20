nanobind_add_module(pymim
    ast.cpp
    def.cpp
    driver.cpp
    error.cpp
    flags.cpp
    lam.cpp
    log.cpp
    parser.cpp
    py.cpp
    sym.cpp
    tuple.cpp
    world.cpp
)

set_target_properties(pymim PROPERTIES LIBRARY_OUTPUT_NAME _mim)
target_link_libraries(pymim PRIVATE libmim)
