#pragma once

#include <mim/axm.h>
#include <mim/world.h>

#include "mim/plug/math/autogen.h"

namespace mim::plug::math {

/// @name Mode
///@{
// clang-format off
/// Allowed optimizations for a specific operation.
enum class Mode : nat_t {
    top      = 0,
    none     = top,                ///< Alias for Mode::none.
    nnan     = 1 << 0,             ///< No NaNs.
                                   ///< Allow optimizations to assume the arguments and result are not NaN.
                                   ///< Such optimizations are required to retain defined behavior over NaNs, but the value of the result is undefined.
    ninf     = 1 << 1,             ///< No Infs.
                                   ///< Allow optimizations to assume the arguments and result are not +/-Inf.
                                   ///< Such optimizations are required to retain defined behavior over +/-Inf, but the value of the result is undefined.
    nsz      = 1 << 2,             ///< No Signed Zeros.
                                   ///< Allow optimizations to treat the sign of a zero argument or result as insignificant.
    arcp     = 1 << 3,             ///< Allow Reciprocal.
                                   ///< Allow optimizations to use the reciprocal of an argument rather than perform division.
    contract = 1 << 4,             ///< Allow floating-point contraction
                                   ///< (e.g. fusing a multiply followed by an addition into a fused multiply-and-add).
    afn      = 1 << 5,             ///< Approximate functions.
                                   ///< Allow substitution of approximate calculations for functions (sin, log, sqrt, etc).
    reassoc  = 1 << 6,             ///< Allow reassociation transformations for floating-point operations.
                                   ///< This may dramatically change results in floating point.
    finite = nnan | ninf,          ///< Mode::nnan `|` Mode::ninf.
    unsafe = nsz | arcp | reassoc, ///< Mode::nsz `|` Mode::arcp `|` Mode::reassoc
    fast   = nnan | ninf | nsz
           | arcp | contract | afn
           | reassoc,              ///< All flags.
    bot    = fast,                 ///< Alias for Mode::fast.
};
// clang-format on

/// Give Mode as mim::plug::math::Mode, mim::nat_t or const Def*.
using VMode = std::variant<Mode, nat_t, const Def*>;

/// mim::plug::math::VMode -> const Def*.
inline const Def* mode(World& w, VMode m) {
    if (auto def = std::get_if<const Def*>(&m)) return *def;
    if (auto nat = std::get_if<nat_t>(&m)) return w.lit_nat(*nat);
    return w.lit_nat(std::to_underlying(std::get<Mode>(m)));
}
///@}

/// @name %%math.F
///@{
inline const Def* type_f(const Def* pe) {
    World& w = pe->world();
    return w.app(w.annex<F>(), pe);
}
inline const Def* type_f(World& w, nat_t p, nat_t e) {
    auto lp = w.lit_nat(p);
    auto le = w.lit_nat(e);
    return type_f(w.tuple({lp, le}));
}
template<nat_t P, nat_t E>
inline auto match_f(const Def* def) {
    if (auto f_ty = Axm::isa<F>(def)) {
        auto [p, e] = f_ty->arg()->projs<2>([](auto op) { return Lit::isa(op); });
        if (p && e && *p == P && *e == E) return f_ty;
    }
    return Axm::IsA<F, App>();
}

inline auto match_f16(const Def* def) { return match_f<10, 5>(def); }
inline auto match_f32(const Def* def) { return match_f<23, 8>(def); }
inline auto match_f64(const Def* def) { return match_f<52, 11>(def); }

inline std::optional<nat_t> isa_f(const Def* def) {
    if (auto f_ty = Axm::isa<F>(def)) {
        if (auto [p, e] = f_ty->arg()->projs<2>([](auto op) { return Lit::isa(op); }); p && e) {
            if (*p == 10 && e == 5) return 16;
            if (*p == 23 && e == 8) return 32;
            if (*p == 52 && e == 11) return 64;
        }
    }
    return {};
}

// clang-format off
template<class R>
const Lit* lit_f(World& w, R val) {
    static_assert(std::is_floating_point_v<R>);
    if (false) {}
    else if constexpr (sizeof(R) == 2) return w.lit(w.annex<F16>(), mim::bitcast<u16>(val));
    else if constexpr (sizeof(R) == 4) return w.lit(w.annex<F32>(), mim::bitcast<u32>(val));
    else if constexpr (sizeof(R) == 8) return w.lit(w.annex<F64>(), mim::bitcast<u64>(val));
    else fe::unreachable();
}

inline const Lit* lit_f(World& w, nat_t width, mim::f64 val) {
    switch (width) {
#if defined(__STDCPP_FLOAT16_T__)
        case 16: assert(mim::f64(mim::f16(val)) == val && "loosing precision"); return lit_f(w, mim::f16(val));
#endif
        case 32: assert(mim::f64(mim::f32(val)) == val && "loosing precision"); return lit_f(w, mim::f32(val));
        case 64: assert(mim::f64(mim::f64(val)) == val && "loosing precision"); return lit_f(w, mim::f64(val));
        default: return nullptr;
    }
}
// clang-format on
///@}

/// @name %%math.arith
///@{
inline const Def* op_rminus(VMode m, const Def* a) {
    World& w = a->world();
    auto s   = isa_f(a->type());
    if (!s) return nullptr;
    if (auto zero = lit_f(w, *s, -0.0)) return w.call(arith::sub, mode(w, m), Defs{zero, a});
    return nullptr;
}
///@}

} // namespace mim::plug::math

namespace mim {

/// @name is_commutative/is_associative
///@{
// clang-format off
constexpr bool is_commutative(plug::math::extrema ) { return true; }
constexpr bool is_commutative(plug::math::arith id) { return id == plug::math::arith::add || id == plug::math::arith::mul; }
constexpr bool is_commutative(plug::math::cmp   id) { return id == plug::math::cmp  ::e   || id == plug::math::cmp  ::ne ; }
constexpr bool is_associative(plug::math::arith id) { return is_commutative(id); }
// clang-format off
///@}

} // namespace mim

#ifndef DOXYGEN
template<> struct fe::is_bit_enum<mim::plug::math::Mode> : std::true_type {};
#endif
