add_executable(mim main.cpp)
add_executable(mim::mim ALIAS mim)
target_link_libraries(mim PRIVATE libmim)

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
