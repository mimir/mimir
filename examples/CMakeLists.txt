add_executable(hello hello.cpp poly.cpp)
add_executable(sccp  sccp_main.cpp sccp_analysis.cpp sccp_transform.cpp)

target_link_libraries(hello PRIVATE libmim)
target_link_libraries(sccp  PRIVATE libmim)

add_dependencies(hello mim_all_plugins)
add_dependencies(sccp  mim_all_plugins)

add_dependencies(mim_all hello)
add_dependencies(mim_all sccp)
