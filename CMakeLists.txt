cmake_minimum_required(VERSION 3.25 FATAL_ERROR)

project(MimIR VERSION 0.2)
set(MIM_VER_SUFFIX "-dev" CACHE STRING "Suffix for version string (e.g., '-dev', '' for release)")

include(CMakeDependentOption)
include(CMakePackageConfigHelpers)
include(CMakePrintHelpers)
include(GNUInstallDirs)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE MIM_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if(NOT MIM_GIT_HASH)
    set(MIM_GIT_HASH "unknown")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
set(MIM_PLUGIN_LIST "" CACHE INTERNAL "MIM_PLUGIN_LIST" FORCE)
set(MIM_PLUGIN_LAYOUT "" CACHE INTERNAL "MIM_PLUGIN_LAYOUT" FORCE)
include(Mim)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23) # global standard for abseil
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}")

option(BUILD_SHARED_LIBS    "Build shared libraries." ON)
option(MIM_BUILD_DOCS       "If ON, build the documentation (requires Doxygen)." OFF)
option(MIM_BUILD_EXAMPLES   "If ON, build examples." OFF)
option(MIM_BUILD_PYTHON     "If ON, build the mim Python package from py/ into ${CMAKE_BINARY_DIR}/.venv." ON)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}; shared libs: ${BUILD_SHARED_LIBS}")

cmake_dependent_option(
    MIM_ENABLE_CHECKS
    "Enable expensive checks (only applicable in Debug mode)" ON
    "CMAKE_BUILD_TYPE STREQUAL Debug" OFF
)

if(WIN32)
    add_compile_definitions(NOMINMAX) # prevents windows.h defining min/max macros
else()
    add_compile_options(-Wall -Wextra)
endif()

add_custom_target(mim_all)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    find_program(MEMORYCHECK_COMMAND valgrind)
    set(VALGRIND_COMMAND_OPTIONS "--leak-check=full --error-exitcode=1")
    option(BUILD_TESTING "Build lit and unit tests." OFF) # needs to be BEFORE "include(CTest)"
    include(CTest)
    cmake_dependent_option(MIM_LIT_WITH_VALGRIND "If ON, the Mim CLI in the lit tests will be run under valgrind." OFF "BUILD_TESTING" OFF)
    set(MIM_LIT_TIMEOUT 20 CACHE STRING "Timout for lit tests.")
    if(BUILD_TESTING)
        include(GoogleTest)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        add_subdirectory(submodules/googletest EXCLUDE_FROM_ALL)
        add_subdirectory(gtest)
        add_subdirectory(lit)
    endif()
endif()

set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_USE_EXTERNAL_GOOGLETEST ON)
set(ABSL_ENABLE_INSTALL ON)
set(FE_ABSL ON)

add_subdirectory(submodules/abseil-cpp)
add_subdirectory(submodules/fe)
add_subdirectory(submodules/lyra)
add_subdirectory(src)

if(MIM_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MIM_BUILD_DOCS)
    find_package(Doxygen REQUIRED dot)
    if(DOXYGEN_FOUND)
        add_subdirectory(docs)
    endif()
endif()

if(MIM_BUILD_PYTHON)
    add_subdirectory(submodules/nanobind)
    add_subdirectory(py)
endif()

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mim-config-version.cmake"
    VERSION ${Mim_VERSION}
    COMPATIBILITY SameMajorVersion
)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mim-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mim-install-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/mim"
)
install(
    TARGETS libmim
    EXPORT mim-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    FILES
        cmake/Mim.cmake
        "${CMAKE_CURRENT_BINARY_DIR}/mim-config-version.cmake"
    DESTINATION
        ${CMAKE_INSTALL_LIBDIR}/cmake/mim
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/mim-install-config.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mim
    RENAME mim-config.cmake
)
install(
    EXPORT mim-targets
    NAMESPACE mim::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mim
)
