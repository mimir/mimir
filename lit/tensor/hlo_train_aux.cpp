// Driver for hlo_train.mim: allocates the one-layer MLP inputs, calls the LLVM-compiled training step, and
// checks the returned scalar against a plain C++ reference forward+backward pass. Parameter order matches
// hlo_train.mim's `main_hlo`: w, b, x, t. Exit code 0 iff every input pattern matches within tolerance.

#include <cmath>
#include <cstddef>
#include <cstdio>

#include <vector>

extern "C" {
float main_hlo(float* w, float* b, float* x, float* t);
}

using Vec = std::vector<float>;

static Vec make(size_t n, int mode) {
    Vec v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = mode == 0 ? static_cast<float>(i % 100) / 100.0f : mode == 1 ? 0.5f : 0.01f * ((i % 7) + 1);
    return v;
}

int main() {
    constexpr int N = 4, I = 8, O = 3;
    constexpr float lr = 0.001f, c = 0.5f;

    int failures = 0;
    for (int mode = 0; mode < 3; ++mode) {
        Vec w = make(I * O, mode), b = make(O, mode), x = make(N * I, mode), t = make(N * O, mode);
        float got = main_hlo(w.data(), b.data(), x.data(), t.data());

        // Reference forward pass: a = tanh(x·w + b); d = c·(a − t).
        Vec z(N * O), a(N * O), d(N * O), rmax(N);
        for (int n = 0; n < N; ++n)
            for (int o = 0; o < O; ++o) {
                float s = 0;
                for (int k = 0; k < I; ++k)
                    s += x[n * I + k] * w[k * O + o];
                z[n * O + o] = s;
            }
        for (int n = 0; n < N; ++n) {
            float mx = -INFINITY;
            for (int o = 0; o < O; ++o)
                mx = std::max(mx, z[n * O + o]);
            rmax[n] = mx;
            for (int o = 0; o < O; ++o) {
                a[n * O + o] = std::tanh(z[n * O + o] + b[o]);
                d[n * O + o] = c * (a[n * O + o] - t[n * O + o]);
            }
        }
        float sel0 = std::isfinite(rmax[0]) ? rmax[0] : 0.0f;

        // Reference backward pass: dW = (dᵀ·x)ᵀ, dx = d·wᵀ, db = Σ_n d.
        Vec dW(I * O);
        for (int i = 0; i < I; ++i)
            for (int o = 0; o < O; ++o) {
                float s = 0;
                for (int n = 0; n < N; ++n)
                    s += d[n * O + o] * x[n * I + i];
                dW[i * O + o] = s;
            }
        float dx00 = 0;
        for (int o = 0; o < O; ++o)
            dx00 += d[0 * O + o] * w[0 * O + o];
        Vec db(O, 0);
        for (int o = 0; o < O; ++o)
            for (int n = 0; n < N; ++n)
                db[o] += d[n * O + o];

        float ref  = (w[0] - lr * dW[0]) + (b[0] - lr * db[0]) + dx00 + sel0;
        float diff = std::abs(got - ref);
        std::printf("mode %d: got=%.7g ref=%.7g diff=%.3g\n", mode, got, ref, diff);
        if (diff > 1e-4f) ++failures;
    }
    return failures == 0 ? 0 : 1;
}
