add_executable(hello hello.cpp poly.cpp)
add_executable(sccp  sccp_main.cpp sccp_analysis.cpp sccp_transform.cpp)

target_link_libraries(hello PRIVATE libmim)
target_link_libraries(sccp  PRIVATE libmim)

add_dependencies(hello mim_all_plugins)
add_dependencies(sccp  mim_all_plugins)

add_dependencies(mim_all hello)
add_dependencies(mim_all sccp)

if(Vulkan_FOUND)
    add_executable(vulkan_runner
        vulkan_runner/main.cpp
        vulkan_runner/obj_loader.cpp
        vulkan_runner/shader_compile.cpp
    )
    target_link_libraries(vulkan_runner PRIVATE Vulkan::Vulkan)

    # Needs the `mim` CLI and every plugin built and sitting next to it at
    # runtime (see exe_dir()/compile_shader() in main.cpp), not libmim
    # itself: it shells out to the built `mim` binary + `spirv-as` rather
    # than linking the compiler.
    add_dependencies(vulkan_runner mim mim_all_plugins)
    add_dependencies(mim_all vulkan_runner)
else()
    message(STATUS "Vulkan not found -- skipping vulkan_runner example")
endif()
