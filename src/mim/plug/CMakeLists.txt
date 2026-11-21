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
if(MIM_X86)
    list(APPEND MIM_PLUGINS x86 x86_isel)
endif()

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

if(MIM_STATIC_PLUGINS)
    mim_static_plugin_registry(mim_static_plugins ${MIM_PLUGIN_LIST})
endif()

add_dependencies(mim_all mim_all_plugins)

if(MIM_X86)
    set(ISEL_RULES "${CMAKE_BINARY_DIR}/include/mim/plug/x86/isel_rules.h")
    set(ISEL_GEN   "${CMAKE_CURRENT_BINARY_DIR}/x86_isel_gen.mim")
    file(GENERATE OUTPUT "${ISEL_GEN}" CONTENT
        "plugin compile;\nplugin x86_isel;\nextern lam _compile (): compile.Phase = x86_isel.gen;\n"
    )
    set(ISEL_PLUGINS ${MIM_PLUGIN_LIST})
    list(REMOVE_ITEM ISEL_PLUGINS x86)
    list(TRANSFORM ISEL_PLUGINS PREPEND "mim_")
    add_custom_command(
        OUTPUT "${ISEL_RULES}"
        COMMAND $<TARGET_FILE:mim> "${ISEL_GEN}" -P "${MIM_PLUGIN_DIR}" -X "x86_isel:o=${ISEL_RULES}"
        DEPENDS mim ${ISEL_PLUGINS} mim_internal_x86 "${MIM_PLUGIN_DIR}/x86.mim" "${MIM_PLUGIN_DIR}/x86_isel.mim"
        COMMENT "Generating the x86 instruction-selection rules"
        VERBATIM
    )
    add_custom_target(mim_x86_isel_rules DEPENDS "${ISEL_RULES}")
    add_dependencies(mim_x86 mim_x86_isel_rules)
endif()

# `--bootstrap` never loads the plugin libraries, so it cannot exercise the normalizers an annex
# relies on: a type error in a .mim only surfaces once something imports it for real.
# `--no-opt` stops after elaboration, since a backend plugin drags in `opt`'s `_default_compile`.
# Elaborating every plugin takes ~1s in total, so do it on every build.
option(MIM_VERIFY_PLUGINS "If ON, elaborate every plugin's .mim with its plugin library loaded." ON)

if(MIM_VERIFY_PLUGINS)
    set(MIM_STAGED_MIMS ${MIM_PLUGIN_LIST})
    list(TRANSFORM MIM_STAGED_MIMS PREPEND "${MIM_PLUGIN_DIR}/")
    list(TRANSFORM MIM_STAGED_MIMS APPEND ".mim")

    set(MIM_VERIFY_STAMPS "")
    foreach(PLUGIN IN LISTS MIM_PLUGIN_LIST)
        set(MIM_VERIFY_MAIN  "${CMAKE_CURRENT_BINARY_DIR}/verify/${PLUGIN}.mim")
        set(MIM_VERIFY_STAMP "${CMAKE_CURRENT_BINARY_DIR}/verify/${PLUGIN}.stamp")
        # `plugin <p>;` loads libmim_<p> first, then parses <p>.mim; running <p>.mim directly would not.
        file(GENERATE OUTPUT "${MIM_VERIFY_MAIN}" CONTENT "plugin ${PLUGIN};\n")
        add_custom_command(
            OUTPUT "${MIM_VERIFY_STAMP}"
            COMMAND $<TARGET_FILE:mim> "${MIM_VERIFY_MAIN}" -P "${MIM_PLUGIN_DIR}" --no-opt
            COMMAND ${CMAKE_COMMAND} -E touch "${MIM_VERIFY_STAMP}"
            DEPENDS mim mim_all_plugins ${MIM_STAGED_MIMS}
            COMMENT "Verifying MimIR plugin '${PLUGIN}'"
            VERBATIM
        )
        list(APPEND MIM_VERIFY_STAMPS "${MIM_VERIFY_STAMP}")
    endforeach()

    add_custom_target(mim_verify_plugins ALL DEPENDS ${MIM_VERIFY_STAMPS})
    add_dependencies(mim_all mim_verify_plugins)
endif()
