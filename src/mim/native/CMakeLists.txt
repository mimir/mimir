# The target-independent phases of the native backends, linked into each of them.
add_library(mim_native STATIC)
target_sources(mim_native
    PRIVATE
        color.cpp
        constrain.cpp
        emit.cpp
        gen.cpp
        isel.cpp
        live.cpp
        spilling.cpp
        subst.cpp
)
target_include_directories(mim_native
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include> # for the autogen.h of the plugins it knows
)
target_link_libraries(mim_native
    PUBLIC
        libmim
        pbqp
)
set_target_properties(mim_native
    PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN 1
)
add_dependencies(mim_native mim_internal_core mim_internal_math mim_internal_mem mim_internal_refly)

# A static backend plugin carries mim_native as a link dependency in the export set.
if(NOT BUILD_SHARED_LIBS)
    install(
        TARGETS mim_native
        EXPORT mim-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
