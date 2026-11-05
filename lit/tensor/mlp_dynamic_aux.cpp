#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <array>
#include <vector>

extern "C" float* mlp_dynamic(uint64_t batch,
                              uint64_t c_in,
                              uint64_t hidden,
                              uint64_t c_out,
                              float* x,
                              float* w1,
                              float* b1,
                              float* w2,
                              float* b2);

static std::vector<float> values(size_t count, size_t seed) {
    std::vector<float> result(count);
    for (size_t i = 0; i != count; ++i)
        result[i] = static_cast<float>(static_cast<int>((i * 7 + seed) % 19) - 9) / 16.0f;
    return result;
}

static std::vector<float> linear(size_t batch,
                                 size_t c_in,
                                 size_t c_out,
                                 const std::vector<float>& x,
                                 const std::vector<float>& w,
                                 const std::vector<float>& b,
                                 bool relu) {
    std::vector<float> result(batch * c_out);
    for (size_t i = 0; i != batch; ++i)
        for (size_t j = 0; j != c_out; ++j) {
            double sum = b[j];
            for (size_t k = 0; k != c_in; ++k)
                sum += double(x[i * c_in + k]) * double(w[j * c_in + k]);
            result[i * c_out + j] = relu ? std::max(0.0f, static_cast<float>(sum)) : static_cast<float>(sum);
        }
    return result;
}

static bool check(size_t batch, size_t c_in, size_t hidden, size_t c_out) {
    auto x     = values(batch * c_in, 3);
    auto w1    = values(hidden * c_in, 5);
    auto b1    = values(hidden, 7);
    auto w2    = values(c_out * hidden, 11);
    auto b2    = values(c_out, 13);
    auto h     = linear(batch, c_in, hidden, x, w1, b1, true);
    auto ref   = linear(batch, hidden, c_out, h, w2, b2, false);
    float* out = mlp_dynamic(batch, c_in, hidden, c_out, x.data(), w1.data(), b1.data(), w2.data(), b2.data());
    bool ok    = true;
    for (size_t i = 0; i != ref.size(); ++i) {
        if (!std::isfinite(out[i]) || std::fabs(out[i] - ref[i]) > 1e-5f + 1e-4f * std::fabs(ref[i])) {
            std::printf("MLP shape (%zu,%zu,%zu,%zu), output %zu: got %g, want %g\n", batch, c_in, hidden, c_out, i,
                        out[i], ref[i]);
            ok = false;
            break;
        }
    }
    std::free(out);
    return ok;
}

int main() {
    constexpr std::array<size_t, 7> batches               = {0, 1, 2, 3, 7, 16, 17};
    constexpr std::array<std::array<size_t, 3>, 8> widths = {
        {{5, 7, 3}, {8, 16, 4}, {1, 1, 1}, {1, 5, 3}, {5, 1, 3}, {0, 7, 3}, {5, 0, 3}, {5, 7, 0}}
    };
    for (auto batch : batches)
        for (auto [c_in, hidden, c_out] : widths)
            if (!check(batch, c_in, hidden, c_out)) return 1;
    std::printf("dynamic MLP: %zu shape combinations passed\n", batches.size() * widths.size());
    return 0;
}
