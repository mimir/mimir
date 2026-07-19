#pragma once

#include <cmath>

namespace vulkan_runner {

/// Column-major 4x4 float matrix, matching SPIR-V/GLSL's convention so it
/// can be memcpy'd directly into a push-constant buffer.
struct Mat4 {
    float m[16]; ///< m[col * 4 + row]

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
        return r;
    }

    static Mat4 mul(const Mat4& a, const Mat4& b) {
        Mat4 r{};
        for (int col = 0; col != 4; ++col)
            for (int row = 0; row != 4; ++row) {
                float sum = 0.f;
                for (int k = 0; k != 4; ++k) sum += a.m[k * 4 + row] * b.m[col * 4 + k];
                r.m[col * 4 + row] = sum;
            }
        return r;
    }

    static Mat4 perspective(float fovy_radians, float aspect, float near, float far) {
        Mat4 r{};
        float f      = 1.f / std::tan(fovy_radians * 0.5f);
        r.m[0]       = f / aspect;
        r.m[5]       = -f; // flip Y: Vulkan's clip space has +Y down, unlike GL
        r.m[10]      = far / (near - far);
        r.m[11]      = -1.f;
        r.m[14]      = (far * near) / (near - far);
        return r;
    }

    static Mat4 look_at(float eye[3], float center[3], float up[3]) {
        float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
        normalize(f);
        float s[3];
        cross(f, up, s);
        normalize(s);
        float u[3];
        cross(s, f, u);

        Mat4 r    = identity();
        r.m[0]    = s[0];
        r.m[4]    = s[1];
        r.m[8]    = s[2];
        r.m[1]    = u[0];
        r.m[5]    = u[1];
        r.m[9]    = u[2];
        r.m[2]    = -f[0];
        r.m[6]    = -f[1];
        r.m[10]   = -f[2];
        r.m[12]   = -dot(s, eye);
        r.m[13]   = -dot(u, eye);
        r.m[14]   = dot(f, eye);
        return r;
    }

private:
    static void normalize(float v[3]) {
        float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-8f) {
            v[0] /= len;
            v[1] /= len;
            v[2] /= len;
        }
    }

    static void cross(const float a[3], const float b[3], float out[3]) {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
};

} // namespace vulkan_runner
