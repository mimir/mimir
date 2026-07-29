// Runtime wrappers for the MimIR `ll` backend.
//
// The functions in this file are implemented in C, compiled to textual LLVM IR by `clang`
// at build time (see `add_mim_runtime` in `cmake/Mim.cmake`), and then either embedded into
// or linked with the module emitted by the `ll` backend (selected via `-X ll:rt=embed|extern`).
//
// This lets the emitter offload complex or platform-dependent lowerings to `clang` instead of
// hand-writing raw LLVM IR; see issue #486.
// Wrappers must expose a flat, scalar ABI (no C aggregates across the boundary) so that the
// emitter can lower a Mim intrinsic to a single `call`.

#include <setjmp.h>
#include <stdint.h>

/// Size in bytes of a `jmp_buf`, used by `%clos.alloc_jmpbuf` to reserve stack space.
/// The size is platform- and libc-dependent, so we let the C compiler compute it rather than
/// hard-coding it in the backend.
int64_t mim_jmpbuf_size(void) { return (int64_t)sizeof(jmp_buf); }
