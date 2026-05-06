set(MIM_PLUGINS
    affine
    autodiff
    clos
    compile
    core
    demo
    direct
    gpu
    math
    matrix
    mem
    opt
    option
    ord
    refly
    regex
    tensor
    tuple
    vec
)

foreach(PLUGIN IN LISTS MIM_PLUGINS)
    add_subdirectory(${PLUGIN})
endforeach()

file(GLOB MIM_EXTRA_PLUGIN_CMAKES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/extra/*/CMakeLists.txt")
foreach(MIM_EXTRA_PLUGIN_CMAKE IN LISTS MIM_EXTRA_PLUGIN_CMAKES)
    get_filename_component(MIM_EXTRA_PLUGIN_DIR "${MIM_EXTRA_PLUGIN_CMAKE}" DIRECTORY)
    get_filename_component(MIM_EXTRA_PLUGIN_NAME "${MIM_EXTRA_PLUGIN_DIR}" NAME)

    if(MIM_EXTRA_PLUGIN_NAME IN_LIST MIM_PLUGINS)
        message(FATAL_ERROR
            "Third-party plugin directory '${MIM_EXTRA_PLUGIN_NAME}' conflicts with an in-tree Mim plugin"
        )
    endif()

    message(STATUS "Adding third-party Mim plugin from '${MIM_EXTRA_PLUGIN_DIR}'")
    add_subdirectory("${MIM_EXTRA_PLUGIN_DIR}" "${CMAKE_BINARY_DIR}/extra/${MIM_EXTRA_PLUGIN_NAME}")
endforeach()

set(MIM_REGISTERED_PLUGINS ${MIM_PLUGIN_LIST})
list(TRANSFORM MIM_REGISTERED_PLUGINS PREPEND "mim_internal_" OUTPUT_VARIABLE MIM_INTERNAL_PLUGINS)

add_custom_target(mim_all_plugins)
foreach(PLUGIN IN LISTS MIM_REGISTERED_PLUGINS)
    add_dependencies(mim_all_plugins mim_${PLUGIN})
    add_dependencies(mim_${PLUGIN} ${MIM_INTERNAL_PLUGINS})
endforeach()

add_dependencies(mim_all mim_all_plugins)
