add_executable(mim-gtest)
target_sources(mim-gtest
    PRIVATE
        cast.cpp
        helpers.cpp
        sets.cpp
        lexer.cpp
        test.cpp
        restricted_dep_types.cpp
        helpers.h
)
target_link_libraries(mim-gtest
    PRIVATE
        gtest_main
        libmim
)
add_dependencies(mim-gtest mim_all_plugins)
gtest_discover_tests(mim-gtest
    TEST_PREFIX "mim."
    DISCOVERY_TIMEOUT 60
)

add_executable(mim-regex-gtest)
target_sources(mim-regex-gtest
    PRIVATE
        automaton.cpp
)
target_link_libraries(mim-regex-gtest
    PRIVATE
        gtest_main
        libmim
        automaton
)
add_dependencies(mim-regex-gtest mim_all_plugins)
gtest_discover_tests(mim-regex-gtest
    TEST_PREFIX "mim-regex-gtest."
    DISCOVERY_TIMEOUT 60
)

add_dependencies(mim_all
    mim-gtest
    mim-regex-gtest
)
