// Runtime wrappers for the MimIR `ll_nvptx` backend.
//
// Like `ll`'s `mim_rt.c`, this file is compiled to textual LLVM IR by `clang` at build time (see
// `add_mim_runtime` in `cmake/Mim.cmake`) and embedded into or linked with the host module emitted
// by the `ll_nvptx` backend (`-X ll_nvptx:rt=embed|extern`); see issue #486.
//
// It intentionally does not include <cuda.h>: the wrappers only touch the CUresult return codes
// that the backend already has in hand, so they stay self-contained and build without the CUDA
// toolkit. Wrappers that need the driver API itself can be added here later.

#include <stdio.h>
#include <stdlib.h>

/// Aborts the program if a CUDA Driver API call returned a non-zero `CUresult`.
/// The backend emits a call to this after every driver call instead of open-coding error handling.
void mim_cu_check(int result) {
    if (result != 0) {
        fprintf(stderr, "MimIR: CUDA driver error: CUresult %d\n", result);
        abort();
    }
}
