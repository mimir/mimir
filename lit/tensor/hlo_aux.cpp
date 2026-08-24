// Driver for hlo_exec.mim: allocates the MLP inputs, calls the LLVM-compiled `main_hlo`, and checks the
// result against a plain C++ reference forward pass. See hlo_exec.mim for the network and the parameter
// order this prototype must match (arg0..arg4, arg6, arg7, arg5).
//
//   h0     = tanh(arg6 · arg0 + arg1)   // 128×784 · 784×1024 -> 128×1024
//   h1     = tanh(h0   · arg2 + arg3)   // 128×1024 · 1024×1024 -> 128×1024
//   logits =      h1   · arg4 + arg5    // 128×1024 · 1024×10 -> 128×10
//   out[i] = max_j logits[i][j]         // -> 128
//
// Exit code: 0 if every output matches the reference within tolerance, 1 otherwise.

#include <cmath>
#include <cstddef>

#include <algorithm>
#include <iostream>
#include <vector>

extern "C" {
// `«s; F32»` tensors lower to pointers to the corresponding nested LLVM array; at the C ABI they are
// plain pointers, so `float*` matches. The result `«128; F32»` comes back as a `float*` to 128 floats.
float* main_hlo(float* arg0, float* arg1, float* arg2, float* arg3, float* arg4, float* arg6, float* arg7, float* arg5);
}

static void initialize_array(std::vector<float>& arr) {
    for (size_t i = 0; i < arr.size(); ++i)
        arr[i] = static_cast<float>(i % 100) / 100.0f;
}

// C = A (M×K) · B (K×N), row-major.
static void matmul(const float* A, const float* B, float* C, size_t M, size_t K, size_t N) {
    for (size_t i = 0; i < M; ++i)
        for (size_t j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (size_t k = 0; k < K; ++k)
                sum += A[i * K + k] * B[k * N + j];
            C[i * N + j] = sum;
        }
}

int main() {
    constexpr size_t B = 128, IN = 784, H = 1024, OUT = 10;

    std::vector<float> arg0(IN * H), arg1(H), arg2(H * H), arg3(H), arg4(H * OUT), arg5(OUT), arg6(B * IN),
        arg7(B * OUT);
    for (auto* a : {&arg0, &arg1, &arg2, &arg3, &arg4, &arg5, &arg6, &arg7})
        initialize_array(*a);

    float* ret = main_hlo(arg0.data(), arg1.data(), arg2.data(), arg3.data(), arg4.data(), arg6.data(), arg7.data(),
                          arg5.data());

    // Reference forward pass.
    std::vector<float> mm0(B * H), h0(B * H), mm1(B * H), h1(B * H), mm2(B * OUT);
    matmul(arg6.data(), arg0.data(), mm0.data(), B, IN, H);
    for (size_t i = 0; i < B * H; ++i)
        h0[i] = std::tanh(mm0[i] + arg1[i % H]);
    matmul(h0.data(), arg2.data(), mm1.data(), B, H, H);
    for (size_t i = 0; i < B * H; ++i)
        h1[i] = std::tanh(mm1[i] + arg3[i % H]);
    matmul(h1.data(), arg4.data(), mm2.data(), B, H, OUT);

    std::vector<float> ref(B);
    for (size_t i = 0; i < B; ++i) {
        float m = -INFINITY;
        for (size_t j = 0; j < OUT; ++j)
            m = std::max(m, mm2[i * OUT + j] + arg5[j]);
        ref[i] = m;
    }

    // f32 matmul accumulation over 1024 terms diverges from the reference only in the last few ulps;
    // a small relative tolerance absorbs that without hiding a real lowering bug.
    int failures  = 0;
    float max_rel = 0.0f;
    for (size_t i = 0; i < B; ++i) {
        float rel = std::abs(ref[i] - ret[i]) / (std::abs(ref[i]) + 1e-6f);
        max_rel   = std::max(max_rel, rel);
        if (rel > 1e-3f) {
            if (failures < 8)
                std::cout << "mismatch at " << i << ": ref " << ref[i] << " got " << ret[i] << " (rel " << rel << ")\n";
            ++failures;
        }
    }
    std::cout << "max relative error: " << max_rel << ", failures: " << failures << std::endl;
    return failures == 0 ? 0 : 1;
}
