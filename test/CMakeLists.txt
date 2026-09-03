add_executable(mim-test)
target_sources(mim-test
    PRIVATE
        cast.cpp
        lexer.cpp
        restricted_dep_types.cpp
        test.cpp
)
target_link_libraries(mim-test
    PRIVATE
        doctest::doctest_with_main
        libmim
)
add_dependencies(mim-test mim_all_plugins)
doctest_discover_tests(mim-test)

add_executable(mim-regex-test)
target_sources(mim-regex-test
    PRIVATE
        automaton.cpp
)
target_link_libraries(mim-regex-test
    PRIVATE
        doctest::doctest_with_main
        libmim
        automaton
)
add_dependencies(mim-regex-test mim_all_plugins)
doctest_discover_tests(mim-regex-test)

add_dependencies(mim_all
    mim-test
    mim-regex-test
)
