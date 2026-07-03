add_library(libmim SHARED)
target_sources(libmim
    PRIVATE
        axm.cpp
        check.cpp
        def.cpp
        dot.cpp
        driver.cpp
        dump.cpp
        lam.cpp
        lattice.cpp
        nest.cpp
        pass.cpp
        phase.cpp
        plugin.cpp
        rewrite.cpp
        rule.cpp
        schedule.cpp
        sexpr.cpp
        tuple.cpp
        world.cpp
        ast/ast.cpp
        ast/bind.cpp
        ast/emit.cpp
        ast/lexer.cpp
        ast/parser.cpp
        ast/stream.cpp
        ast/tok.cpp
        pass/beta_red.cpp
        pass/eta_exp.cpp
        pass/eta_red.cpp
        pass/lam_spec.cpp
        pass/optimize.cpp
        pass/scalarize.cpp
        pass/tail_rec_elim.cpp
        phase/beta_red_phase.cpp
        phase/eta_red_phase.cpp
        phase/eta_exp_phase.cpp
        phase/branch_normalize.cpp
        phase/prefix_cleanup.cpp
        phase/ret_wrap.cpp
        phase/sym_expr_opt.cpp
        util/dbg.cpp
        util/dl.cpp
        util/log.cpp
        util/profile.cpp
        util/sys.cpp
)

set_target_properties(libmim PROPERTIES PREFIX "")
target_compile_features(libmim PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(libmim PUBLIC /utf-8 /wd4146 /wd4245)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(libmim PUBLIC ABSL_CONSUME_DLL)
    endif()
endif()

configure_file(
    "${PROJECT_SOURCE_DIR}/include/mim/config.h.in"
    "${CMAKE_BINARY_DIR}/include/mim/config.h"
)
target_include_directories(libmim
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include> # for config.h
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(libmim
    PUBLIC
        absl::btree
        absl::fixed_array
        absl::inlined_vector
        absl::flat_hash_map absl::flat_hash_set
        absl::node_hash_map absl::node_hash_set
        fe
        ${CMAKE_DL_LIBS}
)

install(
    FILES "${CMAKE_BINARY_DIR}/include/mim/config.h"
    DESTINATION include/mim
)
install(
    DIRECTORY ${PROJECT_SOURCE_DIR}/include/mim
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

add_subdirectory(cli)
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/mim-build-config.cmake.in"
    "${CMAKE_BINARY_DIR}/mim-config.cmake"
    @ONLY
)
set(mim_DIR "${CMAKE_BINARY_DIR}")
add_subdirectory(plug)

add_dependencies(mim_all libmim)
