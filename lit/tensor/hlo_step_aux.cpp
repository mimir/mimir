// Driver for hlo.mim: allocates the full-size two-layer MLP inputs, calls the LLVM-compiled training
// step, and checks the returned scalar against a plain C++ reference forward+backward pass. Parameter
// order matches hlo.mim's `main_hlo`: arg0..arg7 (W1, b1, W2, b2, W3, b3, X, Y).
//
// `main_hlo` returns the sum of the SGD update deltas (old − new = lr·grad) of one element of each of
// the six results; the reference recomputes exactly those elements with the same float operation order,
// so the comparison stays tight (see hlo.mim for the picked indices).
//
// Exit code: 0 if the digest matches the reference within tolerance, 1 otherwise.

#include <cmath>
#include <cstdio>

#include <vector>

extern "C" {
float main_hlo(float* arg0, float* arg1, float* arg2, float* arg3, float* arg4, float* arg5, float* arg6, float* arg7);
}

using Vec = std::vector<float>;

// C = A (M×K) · B (K×N), row-major, ascending-k accumulation like the lowered %tensor.product_2d.
static void matmul(const Vec& A, const Vec& B, Vec& C, size_t M, size_t K, size_t N) {
    for (size_t i = 0; i < M; ++i)
        for (size_t j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (size_t k = 0; k < K; ++k)
                sum += A[i * K + k] * B[k * N + j];
            C[i * N + j] = sum;
        }
}

int main() {
    constexpr size_t B = 128, I = 784, H = 1024, O = 10;
    constexpr float lr = 0.001f;      // %math.conv.f2f (23, 8) 0.001:F64 == 0.001f
    constexpr float c  = -0.0078125f; // v_34 = (-1)/128, constant-folded exactly

    // Small, zero-mean-ish weights keep tanh in its non-saturated regime.
    Vec W1(I * H), b1(H), W2(H * H), b2(H), W3(H * O), b3(O), X(B * I), Y(B * O);
    for (size_t i = 0; i < W1.size(); ++i)
        W1[i] = 0.05f * (static_cast<float>(i % 89) / 89.0f - 0.5f);
    for (size_t i = 0; i < W2.size(); ++i)
        W2[i] = 0.05f * (static_cast<float>(i % 83) / 83.0f - 0.5f);
    for (size_t i = 0; i < W3.size(); ++i)
        W3[i] = 0.1f * (static_cast<float>(i % 79) / 79.0f - 0.5f);
    for (size_t i = 0; i < b1.size(); ++i)
        b1[i] = 0.01f * (static_cast<float>(i % 7) - 3.0f);
    for (size_t i = 0; i < b2.size(); ++i)
        b2[i] = 0.01f * (static_cast<float>(i % 5) - 2.0f);
    for (size_t i = 0; i < b3.size(); ++i)
        b3[i] = 0.01f * (static_cast<float>(i % 3) - 1.0f);
    for (size_t i = 0; i < X.size(); ++i)
        X[i] = 0.05f * (static_cast<float>(i % 97) / 97.0f);
    for (size_t i = 0; i < B; ++i)
        for (size_t j = 0; j < O; ++j)
            Y[i * O + j] = (j == i % O) ? 1.0f : 0.0f;

    float got = main_hlo(W1.data(), b1.data(), W2.data(), b2.data(), W3.data(), b3.data(), X.data(), Y.data());

    // Forward pass: h1 = tanh(X·W1 + b1), h2 = tanh(h1·W2 + b2), logits = h2·W3 + b3.
    Vec h1(B * H), one_m_h1(B * H), h2(B * H), one_m_h2(B * H), logits(B * O);
    {
        Vec mm(B * H);
        matmul(X, W1, mm, B, I, H);
        for (size_t i = 0; i < B * H; ++i) {
            h1[i]       = std::tanh(mm[i] + b1[i % H]);
            one_m_h1[i] = 1.0f - h1[i];
        }
        matmul(h1, W2, mm, B, H, H);
        for (size_t i = 0; i < B * H; ++i) {
            h2[i]       = std::tanh(mm[i] + b2[i % H]);
            one_m_h2[i] = 1.0f - h2[i];
        }
        matmul(h2, W3, logits, B, H, O);
        for (size_t i = 0; i < B * O; ++i)
            logits[i] += b3[i % O];
    }

    // Loss gradient dlog (v_50) via the shifted-softmax block v_18..v_49.
    Vec dlog(B * O);
    for (size_t i = 0; i < B; ++i) {
        float rmax = -INFINITY;
        for (size_t j = 0; j < O; ++j)
            rmax = std::max(rmax, logits[i * O + j]);
        float sel  = std::isfinite(rmax) ? rmax : 0.0f; // v_24
        float ssum = 0.0f;
        Vec e(O);
        for (size_t j = 0; j < O; ++j) {
            e[j] = std::exp(logits[i * O + j] - sel); // v_27
            ssum += e[j];                             // v_28
        }
        float gs = 0.0f;
        for (size_t j = 0; j < O; ++j)
            gs += -(c * Y[i * O + j]);       // v_39 = Σ v_38
        float q   = gs / std::abs(ssum);     // v_41
        float v43 = ssum >= 0.0f ? 0.0f : q; // select(v_32, 0, q)
        float v44 = ssum >= 0.0f ? q : 0.0f; // select(v_32, q, 0)
        float v46 = v44 + -v43;
        for (size_t j = 0; j < O; ++j)
            dlog[i * O + j] = c * Y[i * O + j] + v46 * e[j]; // v_50
    }

    // Backward pass, layer 3 -> 1 (only what the digest needs is reduced to scalars).
    // d2 = dlog·W3 contracted over the class axis (v_56); v_59 = d2·(1−h2) + d2·h2.
    Vec v59(B * H);
    for (size_t i = 0; i < B; ++i)
        for (size_t k = 0; k < H; ++k) {
            float s = 0.0f;
            for (size_t j = 0; j < O; ++j)
                s += dlog[i * O + j] * W3[k * O + j];
            v59[i * H + k] = s * one_m_h2[i * H + k] + s * h2[i * H + k];
        }
    // d1 = v59·W2 contracted over the second hidden axis (v_65); v_68 = v_66 + v_66·h1.
    Vec v68(B * H);
    for (size_t i = 0; i < B; ++i)
        for (size_t k = 0; k < H; ++k) {
            float s = 0.0f;
            for (size_t l = 0; l < H; ++l)
                s += v59[i * H + l] * W2[k * H + l];
            float v66      = s * one_m_h1[i * H + k];
            v68[i * H + k] = v66 + v66 * h1[i * H + k];
        }

    // Digest elements (see hlo.mim): dW1[3][5], db1[7], dW2[11][13], db2[17], dW3[19][2], db3[3].
    auto colsum = [](const Vec& m, size_t stride, size_t col) {
        float s = 0.0f;
        for (size_t i = 0; i < B; ++i)
            s += m[i * stride + col];
        return s;
    };
    float dW1_35 = 0.0f, dW2_11_13 = 0.0f, dW3_19_2 = 0.0f;
    for (size_t i = 0; i < B; ++i)
        dW1_35 += v68[i * H + 5] * X[i * I + 3]; // v_73[3][5] = v_72[5][3]
    for (size_t i = 0; i < B; ++i)
        dW2_11_13 += v59[i * H + 13] * h1[i * H + 11]; // v_64[11][13] = v_63[13][11]
    for (size_t i = 0; i < B; ++i)
        dW3_19_2 += dlog[i * O + 2] * h2[i * H + 19]; // v_55[19][2] = v_54[2][19]
    float db1_7 = colsum(v68, H, 7), db2_17 = colsum(v59, H, 17), db3_3 = colsum(dlog, O, 3);

    // new = old − lr·grad in float, delta = old − new — same rounding path as the compiled code.
    auto delta = [](float old, float grad) {
        float nw = old - lr * grad;
        return old - nw;
    };
    float g[6] = {delta(W1[3 * H + 5], dW1_35),      delta(b1[7], db1_7),
                  delta(W2[11 * H + 13], dW2_11_13), delta(b2[17], db2_17),
                  delta(W3[19 * O + 2], dW3_19_2),   delta(b3[3], db3_3)};
    float ref  = g[0] + (g[1] + (g[2] + (g[3] + (g[4] + g[5])))); // matches the add nesting in hlo.mim

    float scale = 0.0f;
    for (float gi : g)
        scale += std::abs(gi);
    float diff = std::abs(got - ref);
    std::printf("got=%.9g ref=%.9g diff=%.3g scale=%.3g\n", got, ref, diff, scale);
    for (int i = 0; i < 6; ++i)
        std::printf("  g%d=%.9g\n", i, g[i]);

    return diff <= 1e-2f * scale + 1e-6f ? 0 : 1;
}
