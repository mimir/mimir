# Usage: cmake -P run-logged.cmake -- <log> <command> [args...]
#
# Runs <command> with stdout and stderr streamed into <log> instead of the build
# log. A non-zero exit dumps <log> so the failure is not swallowed.

if(CMAKE_ARGC LESS 7)
    message(FATAL_ERROR "usage: cmake -P run-logged.cmake -- <log> <command> [args...]")
endif()

set(LOG "${CMAKE_ARGV4}")
math(EXPR _last "${CMAKE_ARGC} - 1")
set(CMD)
foreach(_i RANGE 5 ${_last})
    # An unescaped ';' would split one argument into several list elements.
    string(REPLACE ";" "\\;" _arg "${CMAKE_ARGV${_i}}")
    list(APPEND CMD "${_arg}")
endforeach()

cmake_path(GET LOG PARENT_PATH _dir)
file(MAKE_DIRECTORY "${_dir}")

execute_process(
    COMMAND ${CMD}
    OUTPUT_FILE "${LOG}"
    ERROR_FILE "${LOG}"
    RESULT_VARIABLE _res
)

if(NOT _res EQUAL 0)
    file(READ "${LOG}" _log)
    message("${_log}")
    list(JOIN CMD " " _cmdline)
    message(FATAL_ERROR "failed (${_res}), see ${LOG}:\n  ${_cmdline}")
endif()
