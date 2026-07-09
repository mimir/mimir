add_executable(mim main.cpp)
target_link_libraries(mim libmim lyra)
if(WIN32)
    # TEMP DIAGNOSTIC: dbghelp for the in-process crash backtrace in main.cpp.
    target_link_libraries(mim dbghelp)
    if(MSVC)
        # TEMP DIAGNOSTIC: Windows default stack is 1 MB (vs 8 MB on Linux). Bump it to test whether the
        # Windows-only, output-less crash is a stack overflow in the recursive phase/SEO traversal.
        target_link_options(mim PRIVATE /STACK:268435456) # 256 MB
    endif()
endif()
set_target_properties(mim PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
install(TARGETS mim EXPORT mim-targets)

add_dependencies(mim_all libmim)
