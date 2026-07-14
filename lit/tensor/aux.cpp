// fun extern main_hlo [arg0: «784; «1024; F32»», arg1: «1024; F32», arg2: «1024; «1024; F32»», arg3: «1024; F32», arg4:
// «1024; «10; F32»», arg5: «10; F32», arg6: «128; «784; F32»», arg7: «128; «10; F32»»]
//  : [«128; F32»]

#include <cstddef>
#include <algorithm>
#include <iostream>
#include <vector>
#include <cstdint>
#include <cmath>

extern "C" {
float* main_hlo(float* arg0, float* arg1, float* arg2, float* arg3, float* arg4, float* arg6, float* arg7, float *arg5);

void print_ptr(void* value) {
    std::cout << std::hex << value << std::endl;
}

void print_int64(std::uint64_t value) {
    std::cout << std::hex << value << std::endl;
}
void print_int32(std::uint32_t value) {
    std::cout << std::hex << value << std::endl;
}
void print_int16(std::uint16_t value) {
    std::cout << std::hex << value << std::endl;
}
void print_int8(std::uint8_t value) {
    std::cout << std::hex << (std::uint16_t)value << std::endl;
}
}

void initialize_array(std::vector<float>& arr) {
    for (size_t i = 0; i < arr.size(); ++i) {
        arr[i] = static_cast<float>(i % 100) / 100.0f; // Initialize with some values
    }
}

void matmul(const float* A, const float* B, float* C, size_t M, size_t K, size_t N) {
    for (size_t i = 0; i < M; ++i) {
        for (size_t j = 0; j < N; ++j) {
            float sum = 0.0f;
            for (size_t k = 0; k < K; ++k) {
                sum += A[i * K + k] * B[k * N + j];
            }
            C[i * N + j] = sum;
        }
    }
}

void broadcast(const float* input, float* output, size_t input_size, size_t output_size) {
    for (size_t i = 0; i < output_size; ++i) {
        output[i] = input[i % input_size]; // Simple broadcasting logic
    }
}

int main(int argc, const char** argv) {
    std::vector<float> arg0(784 * 1024);
    std::vector<float> arg1(1024);
    std::vector<float> arg2(1024 * 1024);
    std::vector<float> arg3(1024);
    std::vector<float> arg4(1024 * 10);
    // float arg5[10];
    std::vector<float> arg5(10);
    std::vector<float> arg6(128 * 784);
    std::vector<float> arg7(128 * 10);

    initialize_array(arg0);
    initialize_array(arg1);
    initialize_array(arg2);
    initialize_array(arg3);
    initialize_array(arg4);
    initialize_array(arg5);
    initialize_array(arg6);
    initialize_array(arg7);

    print_ptr(arg6.data());

    auto ret = main_hlo(arg0.data(), arg1.data(), arg2.data(), arg3.data(), arg4.data(), arg6.data(),
                        arg7.data(), arg5.data());

    for (int i = 0; i < 128; i++) {
        std::cout << ret[i] << " ";
        if(i % 16 == 15) std::cout << std::endl;
    }

    std::vector<float> mmres(128 * 1024);
    matmul(arg6.data(), arg0.data(), mmres.data(), 128, 784, 1024);
    std::vector<float> broadcast_res(128 * 1024);
    broadcast(arg1.data(), broadcast_res.data(), 1024, 128*1024);
    std::vector<float> add_res(128 * 1024);
    for (size_t i = 0; i < add_res.size(); ++i) {
        add_res[i] = std::tanh(mmres[i] + broadcast_res[i]);
    }
    std::vector<float> mm2res(128 * 1024);
    matmul(mmres.data(), arg2.data(), mm2res.data(), 128, 1024, 1024);
    std::vector<float> broadcast2_res(128 * 1024);
    broadcast(arg3.data(), broadcast2_res.data(), 1024, 128*1024);
    std::vector<float> add2_res(128 * 1024);
    for (size_t i = 0; i < add2_res.size(); ++i) {
        add2_res[i] = std::tanh(mm2res[i] + broadcast2_res[i]);
    }
    std::vector<float> mm3res(128 * 10);
    matmul(add2_res.data(), arg4.data(), mm3res.data(), 128, 1024, 10);
    std::vector<float> broadcast3_res(128 * 10);
    broadcast(arg5.data(), broadcast3_res.data(), 10, 128*10);
    std::vector<float> add3_res(128 * 10);
    for (size_t i = 0; i < add3_res.size(); ++i) {
        add3_res[i] = mm3res[i] + broadcast3_res[i];
    }
    std::vector<float> output(128);
    float cnst = -INFINITY;
    for (size_t i = 0; i < output.size(); ++i) {
        float max_val = *std::max_element(add3_res.begin() + i*10, add3_res.begin() + (i+1)*10);
        output[i] = max_val;
    }

    // compare output with ret
    for(size_t i = 0; i < output.size(); ++i) {
        if (std::abs(output[i] - ret[i]) > 1e-5) {
            std::cout << "Mismatch at index " << i << ": expected " << output[i] << ", got " << ret[i] << std::endl;
        }
    }

    return 0;
}
