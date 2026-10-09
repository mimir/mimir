// Driver for attn_exec.mim: calls the LLVM-compiled attention head and checks every output against a scalar reference.
// Exit code: 0 if all outputs match within tolerance, 1 otherwise.

#include <cmath>
#include <cstdio>

#include <vector>

extern "C" {
float* attn(float* x, float* wq, float* wk, float* wv);
float* attn64(float* x, float* wq, float* wk, float* wv);
}

using Mat = std::vector<double>;

static Mat mm(const Mat& a, const Mat& b, size_t n, size_t k, size_t m) {
    Mat c(n * m, 0.0);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < m; ++j)
            for (size_t l = 0; l < k; ++l)
                c[i * m + j] += a[i * k + l] * b[l * m + j];
    return c;
}

static int check(const char* name, float* (*f)(float*, float*, float*, float*), size_t N) {
    constexpr size_t D = 8;
    std::vector<float> x(N * D), wq(D * D), wk(D * D), wv(D * D);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float>(i % 13) / 13.0f - 0.4f;
    for (size_t i = 0; i < wq.size(); ++i) {
        wq[i] = static_cast<float>((7 * i) % 11) / 11.0f - 0.5f;
        wk[i] = static_cast<float>((5 * i) % 9) / 9.0f - 0.5f;
        wv[i] = static_cast<float>((3 * i) % 7) / 7.0f - 0.5f;
    }

    auto xd = Mat(x.begin(), x.end());
    auto q  = mm(xd, Mat(wq.begin(), wq.end()), N, D, D);
    auto k  = mm(xd, Mat(wk.begin(), wk.end()), N, D, D);
    auto v  = mm(xd, Mat(wv.begin(), wv.end()), N, D, D);

    Mat p(N * N);
    for (size_t i = 0; i < N; ++i) {
        double mx = -INFINITY, sum = 0.0;
        for (size_t j = 0; j < N; ++j) {
            double s = 0.0;
            for (size_t l = 0; l < D; ++l)
                s += q[i * D + l] * k[j * D + l];
            p[i * N + j] = s;
            mx           = std::fmax(mx, s);
        }
        for (size_t j = 0; j < N; ++j)
            sum += p[i * N + j] = std::exp(p[i * N + j] - mx);
        for (size_t j = 0; j < N; ++j)
            p[i * N + j] /= sum;
    }
    auto ref = mm(p, v, N, N, D);

    float* out = f(x.data(), wq.data(), wk.data(), wv.data());

    int bad = 0;
    for (size_t i = 0; i < N * D; ++i)
        if (std::fabs(ref[i] - out[i]) > 1e-4 + 1e-4 * std::fabs(ref[i])) {
            if (++bad <= 5) std::printf("%s[%zu,%zu]: got %f want %f\n", name, i / D, i % D, out[i], ref[i]);
        }
    if (bad) std::printf("%s: %d mismatches\n", name, bad);
    return bad;
}

int main() {
    int bad = 0;
    bad += check("attn", attn, 4);
    bad += check("attn64", attn64, 64);
    return bad ? 1 : 0;
}
