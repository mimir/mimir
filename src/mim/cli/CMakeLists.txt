add_executable(mim main.cpp)
target_link_libraries(mim libmim lyra)
if(DEFINED MIM_INSTALL_MIM_RPATH)
    set_target_properties(mim PROPERTIES INSTALL_RPATH "${MIM_INSTALL_MIM_RPATH}")
endif()
install(TARGETS mim EXPORT mim-targets)

add_dependencies(mim_all libmim)
