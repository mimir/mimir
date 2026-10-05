find_package(Python3 COMPONENTS Interpreter)
find_program(MIM_CLANGXX NAMES clang++)
set(MIM_BENCH_ARGS "" CACHE STRING "Extra arguments for bench/run.py, e.g. --torch --filter sq1024")

if(Python3_Interpreter_FOUND AND MIM_CLANGXX)
    set(MIM_BENCH_COMMAND
        ${Python3_EXECUTABLE}
        "${CMAKE_CURRENT_SOURCE_DIR}/run.py"
        --mim $<TARGET_FILE:mim>
        --clangxx "${MIM_CLANGXX}"
        --out "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_custom_target(bench-cpu
        COMMAND ${MIM_BENCH_COMMAND} ${MIM_BENCH_ARGS}
        DEPENDS mim mim_all_plugins
        COMMAND_EXPAND_LISTS
        USES_TERMINAL
    )
    add_custom_target(bench DEPENDS bench-cpu)
endif()
