// Times the externs of the generated bench.mim, checks them against a scalar reference and prints one JSON object per
// line.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <string>
#include <vector>

#include <sys/resource.h>

extern "C" {
#define GEMM(name, M, K, N, R)                     float* name(float*, float*);
#define CONV(name, B, C, H, W, O, KH, KW, S, P, R) float* name(float*, float*);
#include "shapes.inc"
#undef GEMM
#undef CONV
}

using Fn  = float* (*)(float*, float*);
using clk = std::chrono::steady_clock;

/// A network's extern takes one array per parameter; Param says how to fill it.
struct Param {
    size_t size;
    unsigned seed;
    float scale, bias;
};
struct Net {
    const char* op;
    const char* name;
    const char* dims;
    float* (*f)(float**);
    std::vector<Param> params;
    size_t out;
    double flop;
    int reps;
};

struct Gemm {
    const char* name;
    Fn f;
    size_t M, K, N;
    int reps;
};
struct Conv {
    const char* name;
    Fn f;
    size_t B, C, H, W, O, KH, KW, S, P;
    int reps;
};

static std::vector<Gemm> gemms = {
#define GEMM(name, M, K, N, R) {#name, name, M, K, N, R},
#define CONV(...)
#include "shapes.inc"
#undef GEMM
#undef CONV
};
static std::vector<Conv> convs = {
#define GEMM(...)
#define CONV(name, B, C, H, W, O, KH, KW, S, P, R) {#name, name, B, C, H, W, O, KH, KW, S, P, R},
#include "shapes.inc"
#undef GEMM
#undef CONV
};

#include "nets.inc"

static long minor_faults() {
    rusage u;
    getrusage(RUSAGE_SELF, &u);
    return u.ru_minflt;
}

static double faults_per_call = 0;

template<class G>
static double best_secs(G&& g, int reps) {
    double best = 1e30;
    auto f0     = minor_faults();
    for (int r = 0; r < reps; ++r) {
        auto t0 = clk::now();
        g();
        auto t1 = clk::now();
        best    = std::min(best, std::chrono::duration<double>(t1 - t0).count());
    }
    faults_per_call = static_cast<double>(minor_faults() - f0) / reps;
    return best;
}

static void fill(std::vector<float>& v, unsigned seed) {
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = static_cast<float>((seed * i + 3) % 17) / 17.0f - 0.45f;
}

// A network has no scalar reference, so its output must not repeat with fill's period of 17 to catch a misplaced value.
// bench/nets.py repeats this to feed PyTorch the same parameters.
static void fill_net(std::vector<float>& v, const Param& p) {
    for (size_t i = 0; i < v.size(); ++i) {
        auto h = static_cast<uint32_t>(i) * 2654435761u + p.seed * 40503u;
        v[i]   = (static_cast<float>((h >> 16) % 1001) / 1000.0f - 0.5f) * p.scale + p.bias;
    }
}

static double max_err(const float* got, const std::vector<float>& want) {
    double err = 0;
    for (size_t i = 0; i < want.size(); ++i)
        err = std::max(err, static_cast<double>(std::fabs(got[i] - want[i])));
    return err;
}

static void ref_gemm(const float* a, const float* b, float* c, size_t M, size_t K, size_t N) {
    for (size_t i = 0; i < M; ++i) {
        for (size_t j = 0; j < N; ++j)
            c[i * N + j] = 0.0f;
        for (size_t k = 0; k < K; ++k) {
            float x = a[i * K + k];
            for (size_t j = 0; j < N; ++j)
                c[i * N + j] += x * b[k * N + j];
        }
    }
}

static void ref_conv(const float* x, const float* w, float* y, const Conv& s, size_t OH, size_t OW) {
    for (size_t n = 0; n < s.B; ++n)
        for (size_t o = 0; o < s.O; ++o)
            for (size_t oy = 0; oy < OH; ++oy)
                for (size_t ox = 0; ox < OW; ++ox) {
                    float acc = 0.0f;
                    for (size_t c = 0; c < s.C; ++c)
                        for (size_t ky = 0; ky < s.KH; ++ky)
                            for (size_t kx = 0; kx < s.KW; ++kx) {
                                long iy = static_cast<long>(oy * s.S + ky) - static_cast<long>(s.P);
                                long ix = static_cast<long>(ox * s.S + kx) - static_cast<long>(s.P);
                                if (iy < 0 || ix < 0 || iy >= static_cast<long>(s.H) || ix >= static_cast<long>(s.W))
                                    continue;
                                acc += x[((n * s.C + c) * s.H + iy) * s.W + ix]
                                     * w[((o * s.C + c) * s.KH + ky) * s.KW + kx];
                            }
                    y[((n * s.O + o) * OH + oy) * OW + ox] = acc;
                }
}

static bool wanted(int argc, char** argv, const char* name) {
    if (argc < 2) return true;
    for (int i = 1; i < argc; ++i)
        if (!std::strcmp(argv[i], name)) return true;
    return false;
}

static void report(const char* op, const char* name, const char* dims, double flop, double t, double err) {
    std::printf("{\"op\":\"%s\",\"name\":\"%s\",\"dims\":\"%s\",\"ms\":%.4f,\"gflops\":%.2f,\"faults\":%.0f", op, name,
                dims, t * 1e3, flop / t * 1e-9, faults_per_call);
    std::printf(",\"maxerr\":%.3e}\n", err);
    std::fflush(stdout);
}

int main(int argc, char** argv) {
    for (auto& s : gemms) {
        if (!wanted(argc, argv, s.name)) continue;
        std::vector<float> a(s.M * s.K), b(s.K * s.N), c(s.M * s.N);
        fill(a, 5);
        fill(b, 11);
        double flop = 2.0 * s.M * s.N * s.K;
        ref_gemm(a.data(), b.data(), c.data(), s.M, s.K, s.N);
        float* out = s.f(a.data(), b.data());
        double err = max_err(out, c);
        std::free(out);
        double t = best_secs([&] { std::free(s.f(a.data(), b.data())); }, s.reps);
        char dims[64];
        std::snprintf(dims, sizeof dims, "%zux%zux%zu", s.M, s.K, s.N);
        report("gemm", s.name, dims, flop, t, err);
    }
    for (auto& s : convs) {
        if (!wanted(argc, argv, s.name)) continue;
        size_t OH = (s.H + 2 * s.P - s.KH) / s.S + 1, OW = (s.W + 2 * s.P - s.KW) / s.S + 1;
        std::vector<float> x(s.B * s.C * s.H * s.W), w(s.O * s.C * s.KH * s.KW), y(s.B * s.O * OH * OW);
        fill(x, 7);
        fill(w, 13);
        ref_conv(x.data(), w.data(), y.data(), s, OH, OW);
        float* out = s.f(x.data(), w.data());
        double err = max_err(out, y);
        std::free(out);
        double flop = 2.0 * s.B * s.O * OH * OW * s.C * s.KH * s.KW;
        double t    = best_secs([&] { std::free(s.f(x.data(), w.data())); }, s.reps);
        char dims[96];
        std::snprintf(dims, sizeof dims, "%zux%zux%zux%zu*%zux%zux%zu/s%zup%zu", s.B, s.C, s.H, s.W, s.O, s.KH, s.KW,
                      s.S, s.P);
        report("conv", s.name, dims, flop, t, err);
    }
    for (auto& s : nets) {
        if (!wanted(argc, argv, s.name)) continue;
        std::vector<std::vector<float>> ps;
        std::vector<float*> args;
        for (unsigned i = 0; i < s.params.size(); ++i) {
            auto& p = ps.emplace_back(s.params[i].size);
            fill_net(p, s.params[i]);
        }
        for (auto& p : ps)
            args.push_back(p.data());
        // There is no scalar reference for a whole network: run.py compares this output with PyTorch's.
        float* out = s.f(args.data());
        auto path  = std::string(s.name) + ".out";
        if (auto f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(out, sizeof(float), s.out, f);
            std::fclose(f);
        }
        std::free(out);
        double t = best_secs([&] { std::free(s.f(args.data())); }, s.reps);
        report(s.op, s.name, s.dims, s.flop, t, -1);
    }
}
