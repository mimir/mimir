add_mim_plugin(x86_isel
    SOURCES
        x86_isel.cpp
        gen.cpp
        normalizers.cpp
)
add_dependencies(mim_x86_isel mim_internal_x86)
