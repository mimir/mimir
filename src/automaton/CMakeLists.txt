add_library(automaton STATIC)
target_sources(automaton
    PRIVATE
        dfa.cpp
        dfamin.cpp
        nfa2dfa.cpp
        nfa.cpp
)
target_include_directories(automaton
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_compile_features(automaton PUBLIC cxx_std_23)
