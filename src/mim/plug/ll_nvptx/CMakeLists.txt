add_mim_plugin(ll_nvptx
    SOURCES
        ll_nvptx.cpp
        phase/ll_nvptx.cpp
)

add_mim_runtime(ll_nvptx
    SOURCES
        rt/mim_cuda_rt.c
)
