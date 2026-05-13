find_package(Python3 COMPONENTS Interpreter)
set(PYTHON_EXECUTABLE ${Python3_EXECUTABLE})

find_program(MIM_FILECHECK_DEFAULT
    NAMES
        FileCheck
        FileCheck-24
        FileCheck-23
        FileCheck-22
        FileCheck-21
        FileCheck-20
        FileCheck-19
        FileCheck-18
        FileCheck-17
        FileCheck-16
        FileCheck-15
        FileCheck-14
    DOC "FileCheck executable used by lit tests"
)
set(MIM_FILECHECK "${MIM_FILECHECK_DEFAULT}" CACHE STRING "FileCheck command used by lit tests.")
mark_as_advanced(MIM_FILECHECK_DEFAULT)

if(NOT MIM_FILECHECK)
    message(FATAL_ERROR "Could not find FileCheck. Set MIM_FILECHECK to the FileCheck executable used by lit tests.")
endif()

if(MIM_LIT_WITH_VALGRIND)
    set(LIT_VALGRIND_COMMANDLINE "${MEMORYCHECK_COMMAND} ${VALGRIND_COMMAND_OPTIONS} ")
else()
    set(LIT_VALGRIND_COMMANDLINE "")
endif()

set(MIM_LIT_STAGE_FILES "")
set(MIM_LIT_STAGE_DEPENDS "")

function(mim_stage_lit_tree source_root dest_root)
    file(GLOB_RECURSE lit_files CONFIGURE_DEPENDS RELATIVE "${source_root}" "${source_root}/*")
    foreach(rel_path IN LISTS lit_files)
        if(IS_DIRECTORY "${source_root}/${rel_path}")
            continue()
        endif()

        get_filename_component(file_name "${rel_path}" NAME)
        if(file_name STREQUAL "CMakeLists.txt" OR file_name STREQUAL "lit.cfg.py" OR file_name STREQUAL "lit.site.cfg.py"
           OR file_name STREQUAL "lit.site.cfg.py.in")
            continue()
        endif()

        list(APPEND MIM_LIT_STAGE_FILES "${source_root}/${rel_path}|${dest_root}/${rel_path}")
        list(APPEND MIM_LIT_STAGE_DEPENDS "${source_root}/${rel_path}")
    endforeach()

    set(MIM_LIT_STAGE_FILES "${MIM_LIT_STAGE_FILES}" PARENT_SCOPE)
    set(MIM_LIT_STAGE_DEPENDS "${MIM_LIT_STAGE_DEPENDS}" PARENT_SCOPE)
endfunction()

mim_stage_lit_tree("${PROJECT_SOURCE_DIR}/lit" "")

file(GLOB MIM_EXTRA_LIT_DIRS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/extra/*/lit")
foreach(MIM_EXTRA_LIT_DIR IN LISTS MIM_EXTRA_LIT_DIRS)
    if(NOT IS_DIRECTORY "${MIM_EXTRA_LIT_DIR}")
        continue()
    endif()

    get_filename_component(MIM_EXTRA_PLUGIN_DIR "${MIM_EXTRA_LIT_DIR}" DIRECTORY)
    get_filename_component(MIM_EXTRA_PLUGIN_NAME "${MIM_EXTRA_PLUGIN_DIR}" NAME)
    mim_stage_lit_tree("${MIM_EXTRA_LIT_DIR}" "extra/${MIM_EXTRA_PLUGIN_NAME}")
endforeach()

set(MIM_LIT_STAGE_FILES_SCRIPT "")
foreach(MIM_LIT_STAGE_FILE IN LISTS MIM_LIT_STAGE_FILES)
    string(APPEND MIM_LIT_STAGE_FILES_SCRIPT "    \"${MIM_LIT_STAGE_FILE}\"\n")
endforeach()

configure_file(lit.site.cfg.py.in lit.site.cfg.py @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/cmake/stage-lit-tests.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/stage-lit-tests.cmake" @ONLY)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/tests/.stamp"
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/stage-lit-tests.cmake"
    COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_CURRENT_BINARY_DIR}/tests/.stamp"
    DEPENDS
        ${MIM_LIT_STAGE_DEPENDS}
        "${CMAKE_CURRENT_BINARY_DIR}/stage-lit-tests.cmake"
    COMMENT "Staging MimIR lit tests"
)
add_custom_target(mim_lit_tests DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/tests/.stamp")

set(MIM_CTEST_CONFIG_ARGS "")
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND MIM_CTEST_CONFIG_ARGS -C $<CONFIG>)
endif()

set(MIM_LIT_COMMAND
    ${PYTHON_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/lit"
    "${CMAKE_CURRENT_BINARY_DIR}"
    -v
)

add_custom_target(lit
    COMMAND ${MIM_LIT_COMMAND}
    DEPENDS mim mim_all_plugins mim_lit_tests
    COMMAND_EXPAND_LISTS
    USES_TERMINAL
)

add_custom_target(test-all
    COMMAND ${CMAKE_CTEST_COMMAND} --test-dir "${CMAKE_BINARY_DIR}" --output-on-failure ${MIM_CTEST_CONFIG_ARGS}
    COMMAND ${MIM_LIT_COMMAND}
    DEPENDS
        mim-gtest
        mim-regex-gtest
        mim
        mim_all_plugins
        mim_lit_tests
    COMMAND_EXPAND_LISTS
    USES_TERMINAL
)
