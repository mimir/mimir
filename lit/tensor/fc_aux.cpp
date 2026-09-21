// Driver for fc_exec.mim: calls the LLVM-compiled fc layers `relu(x · Wᵀ + b)` (weight stored
// torch-style «c_out, c_in») and checks every output against a scalar reference.
// Exit code: 0 if all outputs match within tolerance, 1 otherwise.

#include <cmath>
#include <cstdio>

#include <vector>

extern "C" {
// `«s; F32»` tensors lower to plain pointers at the C ABI (see hlo_aux.cpp).
float* fc_t(float* x, float* w, float* bias);
float* fc_odd(float* x, float* w, float* bias);
}

static int check(const char* name, float* (*f)(float*, float*, float*), size_t M, size_t K, size_t N) {
    std::vector<float> x(M * K), w(N * K), bias(N);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(i % 13) / 13.0f - 0.4f;
    for (size_t i = 0; i < w.size(); ++i)
        w[i] = static_cast<float>((7 * i) % 11) / 11.0f - 0.5f;
    for (size_t n = 0; n < N; ++n)
        bias[n] = static_cast<float>(n % 5) / 5.0f - 0.4f;

    float* out = f(x.data(), w.data(), bias.data());

    int bad = 0;
    for (size_t m = 0; m < M; ++m)
        for (size_t n = 0; n < N; ++n) {
            double s = 0.0;
            for (size_t k = 0; k < K; ++k)
                s += double(x[m * K + k]) * double(w[n * K + k]);
            float ref = float(s + bias[n]);
            ref       = ref > 0.0f ? ref : 0.0f;
            float got = out[m * N + n];
            if (std::fabs(ref - got) > 1e-4f + 1e-4f * std::fabs(ref)) {
                if (++bad <= 5) std::printf("%s[%zu,%zu]: got %f want %f\n", name, m, n, got, ref);
            }
        }
    if (bad) std::printf("%s: %d mismatches\n", name, bad);
    return bad;
}

int main() {
    int bad = 0;
    bad += check("fc_t", fc_t, 6, 40, 8);
    bad += check("fc_odd", fc_odd, 5, 33, 7);
    return bad ? 1 : 0;
}
