#pragma once

#include <cmath>
#include <cstdint>

#if defined(__STDCPP_FLOAT16_T__)
#    include <stdfloat>
#endif

namespace mim {
// clang-format off

#define MIM_1_8_16_32_64(X) X(1) X(8) X(16) X(32) X(64)
#define MIM_8_16_32_64(X)        X(8) X(16) X(32) X(64)
#if defined(__STDCPP_FLOAT16_T__)
#    define MIM_F16_32_64(X)          X(16) X(32) X(64)
#else
#    define MIM_F16_32_64(X)                X(32) X(64)
#endif

/// @name Aliases for some Base Types
// using CODE1, CODE2, ... here as a workaround for Doxygen
///@{
#define CODE1(i)                   \
    using s ## i =  int ## i ##_t; \
    using u ## i = uint ## i ##_t;
MIM_8_16_32_64(CODE1)
#undef CODE1

using u1 = bool;
#if defined(__STDCPP_FLOAT16_T__)
using f16                           = std::float16_t;
#endif
using f32                           = float;
using f64                           = double;
using level_t                       = u64;
using nat_t                         = u64;
using node_t                        = u8;
using flags_t                       = u64;
using plugin_t                      = u64;
using tag_t                         = u8;
using sub_t                         = u8;
///@}

namespace detail {
template<int> struct w2u_ { using type = void; };
template<int> struct w2s_ { using type = void; };
template<int> struct w2f_ { using type = void; };
template<> struct w2u_<1> { using type = bool; }; ///< Map both signed 1 and unsigned 1 to `bool`.
template<> struct w2s_<1> { using type = bool; }; ///< See above.

#define CODE2(i)                                        \
    template<> struct w2u_<i> { using type = u ## i; }; \
    template<> struct w2s_<i> { using type = s ## i; };
MIM_8_16_32_64(CODE2)
#undef CODE2

#define CODE3(i)                                        \
    template<> struct w2f_<i> { using type = f ## i; };
MIM_F16_32_64(CODE3)
#undef CODE3
} // namespace detail

/// @name Width to Signed/Unsigned/Float
///@{
template<int w> using w2u = typename detail::w2u_<w>::type;
template<int w> using w2s = typename detail::w2s_<w>::type;
template<int w> using w2f = typename detail::w2f_<w>::type;
///@}

/// @name User-Defined Literals
///@{
#define CODE4(i)                                                                        \
    constexpr s ## i operator""_s ## i(unsigned long long int s) { return s ## i(s); } \
    constexpr u ## i operator""_u ## i(unsigned long long int u) { return u ## i(u); }
MIM_8_16_32_64(CODE4)
#undef CODE4

constexpr nat_t operator""_n(unsigned long long int i) { return nat_t(i); }
///@}

/// @name rem
///@{
#if defined(__STDCPP_FLOAT16_T__)
inline f16         rem(f16         a, f16         b) { return std::fmod(a, b); }
#endif
inline float       rem(float       a, float       b) { return std::fmod(a, b); }
inline double      rem(double      a, double      b) { return std::fmod(a, b); }
inline long double rem(long double a, long double b) { return std::fmod(a, b); }
///@}

// clang-format on
} // namespace mim
