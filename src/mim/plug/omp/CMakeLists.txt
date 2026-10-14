add_mim_plugin(omp
    SOURCES
        omp.cpp
        phase/outline.cpp
)

# The `ll` backend splices this module instead of `ll_rt.ll` whenever `omp` is loaded, so it carries `ll`'s wrappers too.
if(MIM_HAS_OPENMP)
    add_mim_runtime(omp
        SOURCES
            ../ll/rt/mim_rt.c
            rt/mim_omp_rt.c
        COMPILE_OPTIONS
            -fopenmp
    )
endif()
