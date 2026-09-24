add_library(libmim) # BUILD_SHARED_LIBS picks the type; a wasm build turns it off, as it has no dlopen
add_library(mim::libmim ALIAS libmim)
target_sources(libmim
    PRIVATE
        axm.cpp
        check.cpp
        def.cpp
        dot.cpp
        driver.cpp
        dump.cpp
        lam.cpp
        nest.cpp
        phase.cpp
        plugin.cpp
        rewrite.cpp
        rule.cpp
        schedule.cpp
        tuple.cpp
        union.cpp
        world.cpp
        ast/ast.cpp
        ast/bind.cpp
        ast/emit.cpp
        ast/lexer.cpp
        ast/parser.cpp
        ast/stream.cpp
        ast/tok.cpp
        phase/adce.cpp
        phase/lam_spec.cpp
        phase/optimize.cpp
        phase/scalarize.cpp
        phase/static_arg_opt.cpp
        phase/beta_red.cpp
        phase/eta_conv.cpp
        phase/branch_normalize.cpp
        phase/single_erasure.cpp
        phase/ret_wrap.cpp
        phase/unload.cpp
)

set_target_properties(libmim PROPERTIES PREFIX "")
target_compile_features(libmim PUBLIC cxx_std_23)

# In a shared libmim every intra-library call to an exported function goes through the PLT by default
# and is opaque to the optimizer.
# Binding those calls locally enables inlining and const-propagation across the whole library.
target_compile_options(libmim
    PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-fno-semantic-interposition>
        # A switch over MutNode without a `default` must handle every mutable node.
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror=switch>
)

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
# fe is an OBJECT library: its objects land in libmim - the only target linking it directly - so that
# the plugins, which all link libmim, resolve fe's symbols there instead of each carrying a copy.
# libmim *is* that shared library, so its own units export fe's symbols rather than import them.
set_target_properties(libmim PROPERTIES DEFINE_SYMBOL fe_EXPORTS)
target_link_libraries(libmim
    PUBLIC
        absl::btree
        absl::fixed_array
        absl::inlined_vector
        absl::flat_hash_map absl::flat_hash_set
        absl::node_hash_map absl::node_hash_set
        fe::fe
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
