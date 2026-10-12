add_mim_plugin(nvptx
    SOURCES
        nvptx.cpp
        phase/nvptx.cpp
)

add_mim_runtime(nvptx
    SOURCES
        rt/mim_cuda_rt.c
)
