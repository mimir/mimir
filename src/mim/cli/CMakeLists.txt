add_executable(mim main.cpp)
target_link_libraries(mim libmim lyra)
set_target_properties(mim PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
if(MSVC)
    # MSVC defaults to a 1 MB stack; bump to 8 MB to match Linux so deep
    # recursive graph rewrites don't overflow.
    target_link_options(mim PRIVATE /STACK:8388608)
endif()
install(TARGETS mim EXPORT mim-targets)

add_dependencies(mim_all libmim)
