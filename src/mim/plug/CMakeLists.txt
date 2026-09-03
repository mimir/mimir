set(MIM_PLUGINS
    affine
    autodiff
    btensor
    buffer
    clos
    compile
    core
    cps
    demo
    gpu
    ll
    ll_nvptx
    math
    mem
    nvptx
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

mim_extra_plugins(CMakeLists.txt MIM_EXTRA_PLUGINS)
foreach(PLUGIN IN LISTS MIM_EXTRA_PLUGINS)
    if(PLUGIN IN_LIST MIM_PLUGINS)
        message(FATAL_ERROR
            "Third-party plugin directory '${PLUGIN}' conflicts with an in-tree Mim plugin"
        )
    endif()

    message(STATUS "Adding third-party Mim plugin from 'extra/${PLUGIN}'")
    add_subdirectory("${PROJECT_SOURCE_DIR}/extra/${PLUGIN}" "${CMAKE_BINARY_DIR}/extra/${PLUGIN}")
endforeach()

list(TRANSFORM MIM_PLUGIN_LIST PREPEND "mim_internal_" OUTPUT_VARIABLE MIM_INTERNAL_PLUGINS)

add_custom_target(mim_all_plugins)
foreach(PLUGIN IN LISTS MIM_PLUGIN_LIST)
    add_dependencies(mim_all_plugins mim_${PLUGIN})
    add_dependencies(mim_${PLUGIN} ${MIM_INTERNAL_PLUGINS})
endforeach()

add_dependencies(mim_all mim_all_plugins)
