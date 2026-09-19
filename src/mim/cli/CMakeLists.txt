add_executable(mim main.cpp)
add_executable(mim::mim ALIAS mim)
target_link_libraries(mim PRIVATE libmim)
if(MIM_STATIC_PLUGINS)
    target_link_libraries(mim PRIVATE mim_static_plugins)
    if(NOT MIM_NATIVE_MIM)
        # Bootstrapping a plugin must not depend on the plugins mim links in.
        add_executable(mim_bootstrap main.cpp)
        target_link_libraries(mim_bootstrap PRIVATE libmim)
    endif()
endif()

# Relative to the executable, so `cmake --install --prefix` yields a working binary;
# a baked-in CMAKE_INSTALL_PREFIX would only ever point at the configure-time prefix.
if(APPLE)
    set(MIM_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
else()
    set(MIM_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
set_target_properties(mim PROPERTIES INSTALL_RPATH "${MIM_INSTALL_RPATH}")
install(TARGETS mim EXPORT mim-targets)

add_dependencies(mim_all mim)
