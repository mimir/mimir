add_mim_plugin(arm_isel
    SOURCES
        arm_isel.cpp
        gen.cpp
        normalizers.cpp
    PRIVATE
        mim_native
)
add_dependencies(mim_arm_isel mim_internal_arm)
