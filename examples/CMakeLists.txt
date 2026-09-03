add_executable(hello hello.cpp poly.cpp)
add_executable(sccp  sccp_main.cpp sccp_analysis.cpp sccp_transform.cpp)

foreach(EXAMPLE IN ITEMS hello sccp)
    target_link_libraries(${EXAMPLE} PRIVATE libmim)
    add_dependencies(${EXAMPLE} mim_all_plugins)
    add_dependencies(mim_all ${EXAMPLE})
endforeach()
