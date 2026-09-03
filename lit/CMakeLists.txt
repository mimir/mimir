find_package(Python3 COMPONENTS Interpreter)

set(MIM_FILECHECK_NAMES "")
foreach(VER RANGE 14 24)
    list(PREPEND MIM_FILECHECK_NAMES FileCheck-${VER})
endforeach()
list(PREPEND MIM_FILECHECK_NAMES FileCheck)

find_program(MIM_FILECHECK_DEFAULT
    NAMES ${MIM_FILECHECK_NAMES}
    DOC "FileCheck executable used by lit tests"
)
set(MIM_FILECHECK "${MIM_FILECHECK_DEFAULT}" CACHE STRING "FileCheck command used by lit tests.")
mark_as_advanced(MIM_FILECHECK_DEFAULT)
# Normalize to forward slashes so the path embedded into lit.site.cfg.py is shell-agnostic.
file(TO_CMAKE_PATH "${MIM_FILECHECK}" MIM_FILECHECK)

if(NOT MIM_FILECHECK)
    message(FATAL_ERROR "Could not find FileCheck. Set MIM_FILECHECK to the FileCheck executable used by lit tests.")
endif()

set(LIT_VALGRIND_COMMANDLINE "")
if(MIM_LIT_WITH_VALGRIND)
    set(LIT_VALGRIND_COMMANDLINE "${MEMORYCHECK_COMMAND} ${VALGRIND_COMMAND_OPTIONS} ")
endif()

set(MIM_LIT_STAGE_FILES "")
set(MIM_LIT_STAGE_DEPENDS "")
set(MIM_LIT_NON_TESTS CMakeLists.txt lit.cfg.py lit.site.cfg.py lit.site.cfg.py.in)

function(mim_stage_lit_tree source_root dest_root)
    file(GLOB_RECURSE lit_files CONFIGURE_DEPENDS RELATIVE "${source_root}" "${source_root}/*")
    foreach(rel_path IN LISTS lit_files)
        cmake_path(GET rel_path FILENAME file_name)
        if(file_name IN_LIST MIM_LIT_NON_TESTS)
            continue()
        endif()

        list(APPEND MIM_LIT_STAGE_FILES "${source_root}/${rel_path}|${dest_root}/${rel_path}")
        list(APPEND MIM_LIT_STAGE_DEPENDS "${source_root}/${rel_path}")
    endforeach()

    set(MIM_LIT_STAGE_FILES "${MIM_LIT_STAGE_FILES}" PARENT_SCOPE)
    set(MIM_LIT_STAGE_DEPENDS "${MIM_LIT_STAGE_DEPENDS}" PARENT_SCOPE)
endfunction()

mim_stage_lit_tree("${PROJECT_SOURCE_DIR}/lit" "")

mim_extra_plugins(lit MIM_EXTRA_PLUGINS)
foreach(PLUGIN IN LISTS MIM_EXTRA_PLUGINS)
    mim_stage_lit_tree("${PROJECT_SOURCE_DIR}/extra/${PLUGIN}/lit" "extra/${PLUGIN}")
endforeach()

set(MIM_LIT_STAGE_FILES_SCRIPT "")
foreach(MIM_LIT_STAGE_FILE IN LISTS MIM_LIT_STAGE_FILES)
    string(APPEND MIM_LIT_STAGE_FILES_SCRIPT "    \"${MIM_LIT_STAGE_FILE}\"\n")
endforeach()

# Detect 16-bit floating-point support once at configure time; mim's fp16 path needs `std::float16_t`.
# Exposed to lit as the `fp16` feature so tests emitting %math.F16 literals can gate via `// REQUIRES: fp16`.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "#include <stdfloat>
     #if !defined(__STDCPP_FLOAT16_T__)
     #    error no std::float16_t
     #endif
     int main() { std::float16_t f = 1.0f16; return int(f); }"
    MIM_HAS_FLOAT16
)

# lit.site.cfg.py.in interpolates the *_SUPPORTED/*_CAN_* values into Python source.
set(MIM_FP16_SUPPORTED False)
if(MIM_HAS_FLOAT16)
    set(MIM_FP16_SUPPORTED True)
endif()

set(MIM_CUDA_CAN_RUN_ON_DEVICE False)
find_program(NVIDIA_SMI nvidia-smi)
if(NVIDIA_SMI)
    execute_process(
        COMMAND ${NVIDIA_SMI} -L
        OUTPUT_VARIABLE SMI_OUT
        RESULT_VARIABLE SMI_RC
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(SMI_RC EQUAL 0 AND SMI_OUT MATCHES "GPU [0-9]+:")
        set(MIM_CUDA_CAN_RUN_ON_DEVICE True)
    endif()
endif()

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

set(MIM_LIT_COMMAND
    ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_SOURCE_DIR}/lit"
    "${CMAKE_CURRENT_BINARY_DIR}"
    -v
)

set(MIM_CTEST_COMMAND
    ${CMAKE_CTEST_COMMAND}
    --test-dir "${CMAKE_BINARY_DIR}"
    --output-on-failure
)
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND MIM_CTEST_COMMAND -C $<CONFIG>)
endif()

add_custom_target(lit
    COMMAND ${MIM_LIT_COMMAND}
    DEPENDS mim mim_all_plugins mim_lit_tests
    COMMAND_EXPAND_LISTS
    USES_TERMINAL
)

add_custom_target(test-all
    COMMAND ${MIM_CTEST_COMMAND}
    COMMAND ${MIM_LIT_COMMAND}
    DEPENDS
        ${MIM_TEST_TARGETS}
        mim
        mim_all_plugins
        mim_lit_tests
    COMMAND_EXPAND_LISTS
    USES_TERMINAL
)
