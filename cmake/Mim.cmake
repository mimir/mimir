include(GNUInstallDirs)

if(NOT DEFINED MIM_PLUGIN_LIST)
    set(MIM_PLUGIN_LIST "" CACHE INTERNAL "MIM_PLUGIN_LIST")
endif()
if(NOT DEFINED MIM_PLUGIN_LAYOUT)
    set(MIM_PLUGIN_LAYOUT "" CACHE INTERNAL "MIM_PLUGIN_LAYOUT")
endif()

if(NOT MIM_TARGET_NAMESPACE)
    set(MIM_TARGET_NAMESPACE "")
endif()

## \page add_mim_plugin_cmake add_mim_plugin
## \brief Registers a new MimIR plugin.
##
## Relative to the plugin's `CMakeLists.txt`, there should be a file
## `<plugin-name>.mim` containing the plugin's annexes.
##
## \code{.cmake}
## add_mim_plugin(<plugin-name>
##     [SOURCES <source>...]
##     [PRIVATE <private-item>...]
##     [INSTALL])
## \endcode
##
## `<plugin-name>` may only contain letters, digits, and underscores, and must
## satisfy MimIR's plugin naming constraints.
##
## The command creates two targets:
## - `mim_internal_<plugin-name>` bootstraps the plugin, generates
##   `<plugin-name>/autogen.h` for the C++ interface used to identify annexes,
##   `<plugin-name>.md` for the documentation, and copies `<plugin-name>.mim`
##   into the build tree.
## - `mim_<plugin-name>` builds the actual `MODULE` library that contains
##   normalizers, passes, and backends. One of the listed source files must
##   export `mim_get_plugin`.
##
## `SOURCES` lists the source files that are compiled into the loadable plugin.
## `PRIVATE` lists additional private link dependencies.
##
## `INSTALL` installs the plugin module, its `.mim` file, and the generated
## `autogen.h`. The export name `mim-targets` must be exported accordingly; see
## CMake's `install(EXPORT ...)` documentation.
##
## Additional target properties can be set afterwards, for example:
## \code{.cmake}
## target_include_directories(mim_<plugin-name> <path>...)
## \endcode
function(add_mim_plugin)
    set(PLUGIN ${ARGV0})

    if(NOT PLUGIN MATCHES "^[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Mim plugin names may only contain letters, digits, and underscores")
    endif()

    string(LENGTH "${PLUGIN}" PLUGIN_LENGTH)
    if(PLUGIN_LENGTH GREATER 8)
        message(FATAL_ERROR "Mim plugin '${PLUGIN}' exceeds the maximum supported length of 8 characters")
    endif()

    cmake_parse_arguments(
        PARSE_ARGV 1        # skip first arg
        PARSED              # prefix of output variables
        "INSTALL"           # options
        ""                  # one-value keywords (none)
        "SOURCES;PRIVATE"   # multi-value keywords
    )

    set(PLUGIN_MIM      ${CMAKE_CURRENT_LIST_DIR}/${PLUGIN}.mim)
    set(OUT_PLUGIN_MIM  ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim/${PLUGIN}.mim)
    set(PLUGIN_MD       ${CMAKE_BINARY_DIR}/docs/plug/${PLUGIN}.md)
    set(AUTOGEN_H       ${CMAKE_BINARY_DIR}/include/mim/plug/${PLUGIN}/autogen.h)
    set(AUTOGEN_PY      ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim/${PLUGIN}.py)

    file(READ "${PLUGIN_MIM}" plugin_file_contents)

    # Strip block comments (/* ... */) — greedy, so repeat if needed
    string(REGEX REPLACE "/\\*[^*]*\\*+([^/*][^*]*\\*+)*/" "" plugin_file_contents "${plugin_file_contents}")

    # Replace all newlines with semicolons to help with list processing
    string(REPLACE "\n" ";" plugin_lines "${plugin_file_contents}")

    file(
        MAKE_DIRECTORY
            ${CMAKE_BINARY_DIR}/docs/plug/
            ${CMAKE_BINARY_DIR}/include/mim/plug/${PLUGIN}
            ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim/
    )

    add_custom_command(
        OUTPUT
            ${AUTOGEN_H}
            ${AUTOGEN_PY}
            ${PLUGIN_MD}
        COMMAND $<TARGET_FILE:${MIM_TARGET_NAMESPACE}mim> ${PLUGIN_MIM} -P "${CMAKE_SOURCE_DIR}/src/mim/plug" -P "${CMAKE_CURRENT_LIST_DIR}/.." --bootstrap
            --output-h ${AUTOGEN_H}
            --output-md ${PLUGIN_MD}
            --output-py ${AUTOGEN_PY}
        MAIN_DEPENDENCY ${PLUGIN_MIM}
        DEPENDS ${MIM_TARGET_NAMESPACE}mim
        COMMENT "Bootstrapping MimIR plugin '${PLUGIN_MIM}'"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${OUT_PLUGIN_MIM}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PLUGIN_MIM} ${OUT_PLUGIN_MIM}
        DEPENDS ${PLUGIN_MIM}
        COMMENT "Copy '${PLUGIN_MIM}' to '${OUT_PLUGIN_MIM}'"
    )

    add_custom_target(mim_internal_${PLUGIN}
        DEPENDS
            ${AUTOGEN_H}
            ${AUTOGEN_PY}
            ${PLUGIN_MD}
            ${OUT_PLUGIN_MIM}
    )

    if(PLUGIN IN_LIST MIM_PLUGIN_LIST)
        message(FATAL_ERROR "Mim plugin '${PLUGIN}' is already registered")
    endif()

    list(APPEND MIM_PLUGIN_LIST "${PLUGIN}")
    string(APPEND MIM_PLUGIN_LAYOUT "<tab type=\"user\" url=\"@ref ${PLUGIN}\" title=\"${PLUGIN}\"/>")

    # populate to globals
    set(MIM_PLUGIN_LIST   "${MIM_PLUGIN_LIST}"   CACHE INTERNAL "MIM_PLUGIN_LIST")
    set(MIM_PLUGIN_LAYOUT "${MIM_PLUGIN_LAYOUT}" CACHE INTERNAL "MIM_PLUGIN_LAYOUT")

    #
    # mim_plugin
    #
    add_library(mim_${PLUGIN} MODULE)
    add_dependencies(mim_${PLUGIN}
        mim_internal_${PLUGIN}
        ${PLUGIN_SOFT_DEPS}
        ${PLUGIN_HARD_DEPS}
    )
    target_sources(mim_${PLUGIN}
        PRIVATE
            ${PARSED_SOURCES}
    )
    target_include_directories(mim_${PLUGIN}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include> # for autogen.h
    )
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/include")
        target_include_directories(mim_${PLUGIN}
            PRIVATE
                "${CMAKE_CURRENT_LIST_DIR}/include"
        )
    endif()
    target_link_libraries(mim_${PLUGIN}
        PRIVATE
            ${PARSED_PRIVATE}
            ${MIM_TARGET_NAMESPACE}libmim
    )
    set_target_properties(mim_${PLUGIN}
        PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN 1
            WINDOWS_EXPORT_ALL_SYMBOLS OFF
            PREFIX "lib" # always use "lib" as prefix regardless of OS/compiler
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim
    )

    #
    # install
    #
    if(${PARSED_INSTALL})
        install(
            TARGETS
                mim_${PLUGIN}
            EXPORT mim-targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/mim
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}/mim
            RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/mim
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mim
        )
        install(
            FILES ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR}/mim/${PLUGIN}.mim
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/mim
        )
        install(
            FILES ${AUTOGEN_H}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mim/plug/${PLUGIN}
        )
        if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/include")
            install(
                DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/include/"
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            )
        endif()
    endif()
endfunction()
