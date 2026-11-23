add_mim_plugin(arm_isel
    SOURCES
        arm_isel.cpp
        gen.cpp
)
add_dependencies(mim_arm_isel mim_internal_arm)
