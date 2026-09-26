add_executable(hello hello.cpp poly.cpp)

foreach(EXAMPLE IN ITEMS hello)
    target_link_libraries(${EXAMPLE} PRIVATE libmim)
    add_dependencies(${EXAMPLE} mim_all_plugins)
    add_dependencies(mim_all ${EXAMPLE})
endforeach()
