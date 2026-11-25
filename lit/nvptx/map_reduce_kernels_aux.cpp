// Driver for map_reduce_kernels.mim: checks every output against a scalar reference.
// Exit code: 0 if all outputs match within tolerance, 1 otherwise.

#include <cmath>
#include <cstdio>

#include <vector>
extern "C" {
float* fc(float* x, float* w, float* b);
float* mm(float* a, float* b);
float* conv_relu(float* x, float* w, float* b);
}
static std::vector<float> fill(size_t n, unsigned mul, unsigned mod, float off) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = float((mul * i) % mod) / float(mod) - off;
    return v;
}
static int bad = 0;
static void cmp(const char* name, size_t i, float got, double want) {
    if (!(std::fabs(want - got) <= 1e-4 + 1e-4 * std::fabs(want)) && ++bad <= 10)
        std::printf("%s[%zu]: got %f want %f\n", name, i, got, want);
}
int main() {
    {
        size_t M = 32, K = 48, N = 64;
        auto x = fill(M * K, 1, 13, 0.4f), w = fill(N * K, 7, 11, 0.5f), b = fill(N, 3, 5, 0.4f);
        float* out = fc(x.data(), w.data(), b.data());
        for (size_t m = 0; m < M; ++m)
            for (size_t n = 0; n < N; ++n) {
                double s = b[n];
                for (size_t k = 0; k < K; ++k)
                    s += double(x[m * K + k]) * w[n * K + k];
                cmp("fc", m * N + n, out[m * N + n], s > 0 ? s : 0);
            }
    }
    {
        size_t M = 32, K = 48, N = 16;
        auto a = fill(M * K, 5, 17, 0.45f), b = fill(K * N, 3, 19, 0.5f);
        float* out = mm(a.data(), b.data());
        for (size_t m = 0; m < M; ++m)
            for (size_t n = 0; n < N; ++n) {
                double s = 0;
                for (size_t k = 0; k < K; ++k)
                    s += double(a[m * K + k]) * b[k * N + n];
                cmp("mm", m * N + n, out[m * N + n], s);
            }
    }
    {
        size_t B = 2, C = 4, H = 10, W = 12, O = 8;
        auto x = fill(B * C * H * W, 7, 23, 0.5f), w = fill(O * C * 9, 5, 13, 0.45f), b = fill(O, 3, 7, 0.3f);
        float* out = conv_relu(x.data(), w.data(), b.data());
        for (size_t n = 0; n < B; ++n)
            for (size_t o = 0; o < O; ++o)
                for (size_t h = 0; h < H; ++h)
                    for (size_t wi = 0; wi < W; ++wi) {
                        double s = b[o];
                        for (size_t c = 0; c < C; ++c)
                            for (int kh = 0; kh < 3; ++kh)
                                for (int kw = 0; kw < 3; ++kw) {
                                    int ih = int(h) + kh - 1, iw = int(wi) + kw - 1;
                                    if (ih < 0 || iw < 0 || ih >= int(H) || iw >= int(W)) continue;
                                    s += double(x[((n * C + c) * H + ih) * W + iw])
                                       * w[((o * C + c) * 3 + kh) * 3 + kw];
                                }
                        cmp("conv", ((n * O + o) * H + h) * W + wi, out[((n * O + o) * H + h) * W + wi], s > 0 ? s : 0);
                    }
    }
    std::printf(bad ? "FAIL: %d mismatches\n" : "OK\n", bad);
    return bad ? 1 : 0;
}
