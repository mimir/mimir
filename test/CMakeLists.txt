# Each doctest executable needs exactly one TU instantiating doctest; generating it keeps that
# boilerplate out of the test sources - especially for auto-discovered third-party plugin tests.
set(MIM_DOCTEST_MAIN "${CMAKE_CURRENT_BINARY_DIR}/doctest_main.cpp")
file(GENERATE
    OUTPUT "${MIM_DOCTEST_MAIN}"
    CONTENT "#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN\n#include <doctest/doctest.h>\n"
)

function(add_mim_test)
    set(TEST ${ARGV0})

    cmake_parse_arguments(
        PARSE_ARGV 1        # skip first arg
        PARSED              # prefix of output variables
        ""                  # options (none)
        ""                  # one-value keywords (none)
        "SOURCES;PRIVATE"   # multi-value keywords
    )

    add_executable(${TEST})
    target_sources(${TEST}
        PRIVATE
            "${MIM_DOCTEST_MAIN}"
            ${PARSED_SOURCES}
    )
    target_link_libraries(${TEST}
        PRIVATE
            ${PARSED_PRIVATE}
            doctest::doctest
            libmim
    )
    add_dependencies(${TEST} mim_all_plugins)
    add_dependencies(mim_all ${TEST})
    doctest_discover_tests(${TEST})

    list(APPEND MIM_TEST_TARGETS ${TEST})
    set(MIM_TEST_TARGETS "${MIM_TEST_TARGETS}" CACHE INTERNAL "MIM_TEST_TARGETS")
endfunction()

set(MIM_TEST_TARGETS "" CACHE INTERNAL "MIM_TEST_TARGETS" FORCE)

add_mim_test(mim-test
    SOURCES
        cast.cpp
        lexer.cpp
        restricted_dep_types.cpp
        test.cpp
)

add_mim_test(mim-regex-test
    SOURCES
        automaton.cpp
    PRIVATE
        automaton
)

file(GLOB MIM_EXTRA_TEST_DIRS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/extra/*/test")
foreach(MIM_EXTRA_TEST_DIR IN LISTS MIM_EXTRA_TEST_DIRS)
    if(NOT IS_DIRECTORY "${MIM_EXTRA_TEST_DIR}")
        continue()
    endif()

    file(GLOB MIM_EXTRA_TEST_SOURCES CONFIGURE_DEPENDS "${MIM_EXTRA_TEST_DIR}/*.cpp")
    if(NOT MIM_EXTRA_TEST_SOURCES)
        continue()
    endif()

    get_filename_component(MIM_EXTRA_PLUGIN_DIR "${MIM_EXTRA_TEST_DIR}" DIRECTORY)
    get_filename_component(MIM_EXTRA_PLUGIN_NAME "${MIM_EXTRA_PLUGIN_DIR}" NAME)
    set(MIM_EXTRA_TEST "mim-${MIM_EXTRA_PLUGIN_NAME}-test")

    message(STATUS "Adding third-party Mim unit tests '${MIM_EXTRA_TEST}' from '${MIM_EXTRA_TEST_DIR}'")
    add_mim_test(${MIM_EXTRA_TEST} SOURCES ${MIM_EXTRA_TEST_SOURCES})

    if(EXISTS "${MIM_EXTRA_PLUGIN_DIR}/include")
        target_include_directories(${MIM_EXTRA_TEST} PRIVATE "${MIM_EXTRA_PLUGIN_DIR}/include")
    endif()
endforeach()
