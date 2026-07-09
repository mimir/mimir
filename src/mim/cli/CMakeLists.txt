add_executable(mim main.cpp)
target_link_libraries(mim libmim lyra)
if(WIN32)
    # TEMP DIAGNOSTIC: dbghelp for the in-process crash backtrace in main.cpp.
    target_link_libraries(mim dbghelp)
endif()
set_target_properties(mim PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
install(TARGETS mim EXPORT mim-targets)

add_dependencies(mim_all libmim)
