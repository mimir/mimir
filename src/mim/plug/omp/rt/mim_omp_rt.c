// Runtime wrapper for the MimIR `omp` plugin; see `ll/rt/mim_rt.c` for how such wrappers reach the output.

#include <omp.h>
#include <stdint.h>

/// Runs `body` over `[begin, end)` with stride `step`, handing every thread one contiguous chunk `[lo, hi)`.
/// Each thread's chunk holds the same number of iterations (up to one), so a uniform loop nest stays balanced.
void mim_par_for(int64_t begin, int64_t end, int64_t step, void (*body)(void*, int64_t, int64_t), void* env) {
    if (step <= 0 || begin >= end) return;
    int64_t n = (end - begin + step - 1) / step;

#pragma omp parallel
    {
        int64_t t     = omp_get_thread_num();
        int64_t nt    = omp_get_num_threads();
        int64_t chunk = n / nt;
        int64_t rem   = n % nt;
        int64_t lo    = t * chunk + (t < rem ? t : rem);
        int64_t hi    = lo + chunk + (t < rem ? 1 : 0);
        if (lo < hi) body(env, begin + lo * step, begin + hi * step);
    }
}
