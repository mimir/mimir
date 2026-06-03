#include "mim/plug/math/math.h"

namespace mim::plug::math {

namespace {

template<class F>
std::optional<u64> dispatch_float_width(nat_t width, F&& f) {
    switch (width) {
#define CODE(i) \
    case i: return f.template operator()<i>();
        MIM_F16_32_64(CODE)
#undef CODE
        default: return {};
    }
}

template<class F>
std::optional<u64> dispatch_int_width(nat_t width, F&& f) {
    switch (width) {
#define CODE(i) \
    case i: return f.template operator()<i>();
        MIM_1_8_16_32_64(CODE)
#undef CODE
        default: return {};
    }
}

template<nat_t w, class F>
std::optional<u64> fold_float_unary_bits(u64 a, F&& f) {
    using T = w2f<w>;
    auto x  = bitcast_resize<T>(a);
    return bitcast_resize<u64>(static_cast<T>(f(x)));
}

template<nat_t w, class F>
std::optional<u64> fold_float_binary_bits(u64 a, u64 b, F&& f) {
    using T = w2f<w>;
    auto x  = bitcast_resize<T>(a);
    auto y  = bitcast_resize<T>(b);
    return bitcast_resize<u64>(static_cast<T>(f(x, y)));
}

template<nat_t w>
constexpr long double signed_min() {
    if constexpr (w == 1)
        return -1.0L;
    else
        return static_cast<long double>(std::numeric_limits<w2s<w>>::min());
}

template<nat_t w>
constexpr long double signed_max() {
    if constexpr (w == 1)
        return 0.0L;
    else
        return static_cast<long double>(std::numeric_limits<w2s<w>>::max());
}

template<nat_t w>
constexpr long double unsigned_max() {
    return static_cast<long double>(std::numeric_limits<w2u<w>>::max());
}

template<nat_t w>
std::optional<u64> encode_signed(long double x) {
    if constexpr (w == 1) {
        if (x == -1.0L) return 1_u64;
        if (x == 0.0L) return 0_u64;
        return {};
    } else {
        return bitcast_resize<u64>(static_cast<w2s<w>>(x));
    }
}

template<nat_t w>
std::optional<u64> encode_unsigned(long double x) {
    return bitcast_resize<u64>(static_cast<w2u<w>>(x));
}

template<nat_t w>
long double decode_signed(u64 a) {
    if constexpr (w == 1)
        return bitcast_resize<bool>(a) ? -1.0L : 0.0L;
    else
        return static_cast<long double>(bitcast_resize<w2s<w>>(a));
}

template<nat_t w>
long double decode_unsigned(u64 a) {
    return static_cast<long double>(bitcast_resize<w2u<w>>(a));
}

template<nat_t w, class F>
std::optional<u64> fold_float_to_signed_bits(F x) {
    if (!std::isfinite(x)) return {};

    auto truncated = std::trunc(static_cast<long double>(x));
    if (truncated < signed_min<w>() || truncated > signed_max<w>()) return {};
    return encode_signed<w>(truncated);
}

template<nat_t w, class F>
std::optional<u64> fold_float_to_unsigned_bits(F x) {
    if (!std::isfinite(x)) return {};

    auto truncated = std::trunc(static_cast<long double>(x));
    if (truncated < 0.0L || truncated > unsigned_max<w>()) return {};
    return encode_unsigned<w>(truncated);
}

// clang-format off
template<class Id, Id id, nat_t w>
std::optional<u64> fold_unary_lit(u64 a) {
    if constexpr (std::is_same_v<Id, tri>) {
        if constexpr (false) {}
        else if constexpr (id == tri:: sin ) return fold_float_unary_bits<w>(a, [](auto x) { return std:: sin (x); });
        else if constexpr (id == tri:: cos ) return fold_float_unary_bits<w>(a, [](auto x) { return std:: cos (x); });
        else if constexpr (id == tri:: tan ) return fold_float_unary_bits<w>(a, [](auto x) { return std:: tan (x); });
        else if constexpr (id == tri:: sinh) return fold_float_unary_bits<w>(a, [](auto x) { return std:: sinh(x); });
        else if constexpr (id == tri:: cosh) return fold_float_unary_bits<w>(a, [](auto x) { return std:: cosh(x); });
        else if constexpr (id == tri:: tanh) return fold_float_unary_bits<w>(a, [](auto x) { return std:: tanh(x); });
        else if constexpr (id == tri::asin ) return fold_float_unary_bits<w>(a, [](auto x) { return std::asin (x); });
        else if constexpr (id == tri::acos ) return fold_float_unary_bits<w>(a, [](auto x) { return std::acos (x); });
        else if constexpr (id == tri::atan ) return fold_float_unary_bits<w>(a, [](auto x) { return std::atan (x); });
        else if constexpr (id == tri::asinh) return fold_float_unary_bits<w>(a, [](auto x) { return std::asinh(x); });
        else if constexpr (id == tri::acosh) return fold_float_unary_bits<w>(a, [](auto x) { return std::acosh(x); });
        else if constexpr (id == tri::atanh) return fold_float_unary_bits<w>(a, [](auto x) { return std::atanh(x); });
        else fe::unreachable();
    } else if constexpr (std::is_same_v<Id, rt>) {
        if constexpr (false) {}
        else if constexpr (id == rt::sq) return fold_float_unary_bits<w>(a, [](auto x) { return std::sqrt(x); });
        else if constexpr (id == rt::cb) return fold_float_unary_bits<w>(a, [](auto x) { return std::cbrt(x); });
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, exp>) {
        if constexpr (false) {}
        else if constexpr (id == exp::exp)   return fold_float_unary_bits<w>(a, [](auto x) { return std::exp(x); });
        else if constexpr (id == exp::exp2)  return fold_float_unary_bits<w>(a, [](auto x) { return std::exp2(x); });
        else if constexpr (id == exp::exp10) return fold_float_unary_bits<w>(a, [](auto x) { return std::pow(decltype(x)(10), x); });
        else if constexpr (id == exp::log)   return fold_float_unary_bits<w>(a, [](auto x) { return std::log(x); });
        else if constexpr (id == exp::log2)  return fold_float_unary_bits<w>(a, [](auto x) { return std::log2(x); });
        else if constexpr (id == exp::log10) return fold_float_unary_bits<w>(a, [](auto x) { return std::log10(x); });
        else fe::unreachable();
    } else if constexpr (std::is_same_v<Id, er>) {
        if constexpr (false) {}
        else if constexpr (id == er::f ) return fold_float_unary_bits<w>(a, [](auto x) { return std::erf (x); });
        else if constexpr (id == er::fc) return fold_float_unary_bits<w>(a, [](auto x) { return std::erfc(x); });
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, gamma>) {
        if constexpr (false) {}
        else if constexpr (id == gamma::t) return fold_float_unary_bits<w>(a, [](auto x) { return std::tgamma(x); });
        else if constexpr (id == gamma::l) return fold_float_unary_bits<w>(a, [](auto x) { return std::lgamma(x); });
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, round>) {
        if constexpr (false) {}
        else if constexpr (id == round::f) return fold_float_unary_bits<w>(a, [](auto x) { return std::floor(x); });
        else if constexpr (id == round::c) return fold_float_unary_bits<w>(a, [](auto x) { return std::ceil(x); });
        else if constexpr (id == round::r) return fold_float_unary_bits<w>(a, [](auto x) { return std::round(x); });
        else if constexpr (id == round::t) return fold_float_unary_bits<w>(a, [](auto x) { return std::trunc(x); });
        else static_assert(false, "missing sub tag");
    } else {
        static_assert(false, "missing tag");
    }
}
// clang-format on

template<class Id, nat_t w>
std::optional<u64> fold_unary_lit(u64 a) {
    if constexpr (std::is_same_v<Id, abs>)
        return fold_float_unary_bits<w>(a, [](auto x) { return std::abs(x); });
    else
        static_assert(false, "missing tag");
}

template<class Id>
const Def* fold(World& world, const Def* type, const Def* a) {
    if (a->isa<Bot>()) return world.bot(type);

    if (auto la = Lit::isa(a))
        if (auto width = isa_f(a->type()))
            if (auto res = dispatch_float_width(*width, [&]<nat_t w>() { return fold_unary_lit<Id, w>(*la); }))
                return world.lit(type, *res);

    return nullptr;
}

template<class Id, Id id, nat_t w>
std::optional<u64> fold_binary_lit(u64 a, u64 b) {
    using T = w2f<w>;
    auto x  = bitcast_resize<T>(a);
    auto y  = bitcast_resize<T>(b);

    if constexpr (std::is_same_v<Id, arith>) {
        // clang-format off
        if constexpr (false) {}
        else if constexpr (id == arith::add) return bitcast_resize<u64>(static_cast<T>(x + y));
        else if constexpr (id == arith::sub) return bitcast_resize<u64>(static_cast<T>(x - y));
        else if constexpr (id == arith::mul) return bitcast_resize<u64>(static_cast<T>(x * y));
        else if constexpr (id == arith::div) return bitcast_resize<u64>(static_cast<T>(x / y));
        else if constexpr (id == arith::rem) return bitcast_resize<u64>(static_cast<T>(rem(x, y)));
        else static_assert(false, "missing sub tag");
        // clang-format on
    } else if constexpr (std::is_same_v<Id, math::extrema>) {
        T result;

        if (x == T(-0.0) && y == T(+0.0)) {
            result = (id == extrema::fmin || id == extrema::ieee754min) ? x : y;
        } else if (x == T(+0.0) && y == T(-0.0)) {
            result = (id == extrema::fmin || id == extrema::ieee754min) ? y : x;
        } else if constexpr (id == extrema::fmin || id == extrema::fmax) {
            result = id == extrema::fmin ? std::fmin(x, y) : std::fmax(x, y);
        } else if constexpr (id == extrema::ieee754min || id == extrema::ieee754max) {
            if (std::isnan(x))
                result = x;
            else if (std::isnan(y))
                result = y;
            else
                result = id == extrema::ieee754min ? std::fmin(x, y) : std::fmax(x, y);
        } else {
            static_assert(false, "missing sub tag");
        }

        return bitcast_resize<u64>(result);
    } else if constexpr (std::is_same_v<Id, pow>) {
        return bitcast_resize<u64>(static_cast<T>(std::pow(x, y)));
    } else if constexpr (std::is_same_v<Id, cmp>) {
        using std::isunordered;
        bool result = false;
        result |= ((id & cmp::u) != cmp::f) && isunordered(x, y);
        result |= ((id & cmp::g) != cmp::f) && x > y;
        result |= ((id & cmp::l) != cmp::f) && x < y;
        result |= ((id & cmp::e) != cmp::f) && x == y;
        return u64(result);
    } else {
        static_assert(false, "missing tag");
    }
}

template<class Id, Id id>
const Def* fold(World& world, const Def* type, const Def* a) {
    if (a->isa<Bot>()) return world.bot(type);

    if (auto la = Lit::isa(a))
        if (auto width = isa_f(a->type()))
            if (auto res = dispatch_float_width(*width, [&]<nat_t w>() { return fold_unary_lit<Id, id, w>(*la); }))
                return world.lit(type, *res);

    return nullptr;
}

// Note that @p a and @p b are passed by reference as fold also commutes if possible.
template<class Id, Id id>
const Def* fold(World& world, const Def* type, const Def*& a, const Def*& b) {
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type);

    if (auto la = Lit::isa(a))
        if (auto lb = Lit::isa(b))
            if (auto width = isa_f(a->type()))
                if (auto res
                    = dispatch_float_width(*width, [&]<nat_t w>() { return fold_binary_lit<Id, id, w>(*la, *lb); }))
                    return world.lit(type, *res);

    if (is_commutative(id) && Def::greater(a, b)) std::swap(a, b);
    return nullptr;
}

/// Reassociates @p a und @p b according to following rules.
/// We use the following naming convention while literals are prefixed with an 'l':
/// ```
///     a    op     b
/// (x op y) op (z op w)
///
/// (1)     la    op (lz op w) -> (la op lz) op w
/// (2) (lx op y) op (lz op w) -> (lx op lz) op (y op w)
/// (3)      a    op (lz op w) ->  lz op (a op w)
/// (4) (lx op y) op      b    ->  lx op (y op b)
/// ```
template<class Id>
const Def* reassociate(Id id, World& world, [[maybe_unused]] const App* ab, const Def* a, const Def* b) {
    if (!is_associative(id)) return nullptr;

    auto xy     = Axm::isa<Id>(id, a);
    auto zw     = Axm::isa<Id>(id, b);
    auto la     = a->isa<Lit>();
    auto [x, y] = xy ? xy->template args<2>() : std::array<const Def*, 2>{nullptr, nullptr};
    auto [z, w] = zw ? zw->template args<2>() : std::array<const Def*, 2>{nullptr, nullptr};
    auto lx     = Lit::isa(x);
    auto lz     = Lit::isa(z);

    // build mode for all new ops by using the least upper bound of all involved apps
    auto mode       = std::to_underlying(Mode::bot);
    auto check_mode = [&](const App* app) {
        auto app_m = Lit::isa(app->arg(0));
        if (!app_m || !fe::has_flag(static_cast<Mode>(*app_m), Mode::reassoc)) return false;
        mode &= *app_m; // least upper bound
        return true;
    };

    if (!check_mode(ab)) return nullptr;
    if (lx && !check_mode(xy->decurry())) return nullptr;
    if (lz && !check_mode(zw->decurry())) return nullptr;

    auto make_op = [&](const Def* a, const Def* b) { return world.call(id, mode, Defs{a, b}); };

    if (la && lz) return make_op(make_op(a, z), w);             // (1)
    if (lx && lz) return make_op(make_op(x, z), make_op(y, w)); // (2)
    if (lz) return make_op(z, make_op(a, w));                   // (3)
    if (lx) return make_op(x, make_op(y, b));                   // (4)
    return nullptr;
}

template<conv id, nat_t sw, nat_t dw>
std::optional<u64> fold_conv_lit(u64 a) {
    using S = w2f<sw>;
    using D = w2f<dw>;
    // clang-format off
    if constexpr (false) {}
    else if constexpr (id == conv::s2f) return bitcast_resize<u64>(static_cast<D>(decode_signed<sw>(a)));
    else if constexpr (id == conv::u2f) return bitcast_resize<u64>(static_cast<D>(decode_unsigned<sw>(a)));
    else if constexpr (id == conv::f2s) return fold_float_to_signed_bits<dw>(bitcast_resize<S>(a));
    else if constexpr (id == conv::f2u) return fold_float_to_unsigned_bits<dw>(bitcast_resize<S>(a));
    else if constexpr (id == conv::f2f) return bitcast_resize<u64>(static_cast<D>(bitcast_resize<S>(a)));
    else static_assert(false, "missing sub tag");
    // clang-format on
}

template<conv id, nat_t sw>
std::optional<u64> fold_conv_dst(nat_t dw, u64 a) {
    if constexpr (id == conv::s2f || id == conv::u2f || id == conv::f2f)
        return dispatch_float_width(dw, [&]<nat_t d>() { return fold_conv_lit<id, sw, d>(a); });
    else
        return dispatch_int_width(dw, [&]<nat_t d>() { return fold_conv_lit<id, sw, d>(a); });
}

template<conv id>
std::optional<u64> fold_conv(nat_t sw, nat_t dw, u64 a) {
    if constexpr (id == conv::s2f || id == conv::u2f)
        return dispatch_int_width(sw, [&]<nat_t s>() { return fold_conv_dst<id, s>(dw, a); });
    else
        return dispatch_float_width(sw, [&]<nat_t s>() { return fold_conv_dst<id, s>(dw, a); });
}

} // namespace

template<arith id>
const Def* normalize_arith(const Def* type, const Def* c, const Def* arg) {
    auto& world = type->world();
    auto callee = c->as<App>();
    auto [a, b] = arg->projs<2>();
    auto mode   = callee->arg();
    auto lm     = Lit::isa(mode);
    auto w      = isa_f(a->type());

    if (auto result = fold<arith, id>(world, type, a, b)) return result;

    // clang-format off
    // TODO check mode properly
    if (w && lm && static_cast<Mode>(*lm) == Mode::fast) {
        auto zero = lit_f(world, *w, 0.0);
        auto one  = lit_f(world, *w, 1.0);
        auto two  = lit_f(world, *w, 2.0);

        if (auto la = a->isa<Lit>()) {
            if (zero && la == zero) {
                switch (id) {
                    case arith::add: return b;  // 0 + b -> b
                    case arith::sub: break;
                    case arith::mul: return la; // 0 * b -> 0
                    case arith::div: return la; // 0 / b -> 0
                    case arith::rem: return la; // 0 % b -> 0
                }
            }

            if (one && la == one) {
                switch (id) {
                    case arith::add: break;
                    case arith::sub: break;
                    case arith::mul: return b;  // 1 * b -> b
                    case arith::div: break;
                    case arith::rem: break;
                }
            }
        }

        if (auto lb = b->isa<Lit>()) {
            if (zero && lb == zero) {
                switch (id) {
                    case arith::sub: return a;  // a - 0 -> a
                    case arith::div: break;
                    case arith::rem: break;
                    default: fe::unreachable();
                    // add, mul are commutative, the literal has been normalized to the left
                }
            }
        }

        if (a == b) {
            switch (id) {
                case arith::add: if (two ) return world.call(arith::mul, mode, Defs{two , a}); break; // a + a -> 2 * a
                case arith::sub: if (zero) return zero; break;                                          // a - a -> 0
                case arith::mul: break;
                case arith::div: if (one ) return one ; break;                                          // a / a -> 1
                case arith::rem: break;
            }
        }
    }
    // clang-format on

    if (auto res = reassociate<arith>(id, world, callee, a, b)) return res;

    return world.raw_app(type, callee, {a, b});
}

template<extrema id>
const Def* normalize_extrema(const Def* type, const Def* c, const Def* arg) {
    auto& world = type->world();
    auto callee = c->as<App>();
    auto [a, b] = arg->projs<2>();
    auto m      = callee->arg();
    auto lm     = Lit::isa(m);
    // TODO commute

    if (auto lit = fold<extrema, id>(world, type, a, b)) return lit;

    if (lm && (fe::has_flag(static_cast<Mode>(*lm), Mode::nnan) || fe::has_flag(static_cast<Mode>(*lm), Mode::nsz))) {
        switch (id) {
            case extrema::ieee754min: return world.call(extrema::fmin, m, Defs{a, b});
            case extrema::ieee754max: return world.call(extrema::fmax, m, Defs{a, b});
            default: break;
        }
    }

    return world.raw_app(type, c, {a, b});
}

template<tri id>
const Def* normalize_tri(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<tri, id>(world, type, arg)) return lit;
    return {};
}

const Def* normalize_pow(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    auto [a, b] = arg->projs<2>();
    if (auto lit = fold<pow, /*dummy*/ pow(0)>(world, type, a, b)) return lit;
    return {};
}

template<rt id>
const Def* normalize_rt(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<rt, id>(world, type, arg)) return lit;
    return {};
}

template<exp id>
const Def* normalize_exp(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<exp, id>(world, type, arg)) return lit;
    return {};
}

template<er id>
const Def* normalize_er(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<er, id>(world, type, arg)) return lit;
    return {};
}

template<gamma id>
const Def* normalize_gamma(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<gamma, id>(world, type, arg)) return lit;
    return {};
}

template<cmp id>
const Def* normalize_cmp(const Def* type, const Def* c, const Def* arg) {
    auto& world = type->world();
    auto callee = c->as<App>();
    auto [a, b] = arg->projs<2>();

    if (auto result = fold<cmp, id>(world, type, a, b)) return result;
    if (id == cmp::f) return world.lit_ff();
    if (id == cmp::t) return world.lit_tt();

    return world.raw_app(type, callee, {a, b});
}

template<conv id>
const Def* normalize_conv(const Def* dst_t, const Def*, const Def* x) {
    auto& world = dst_t->world();
    auto s_t    = x->type()->as<App>();
    auto d_t    = dst_t->as<App>();
    auto s      = s_t->arg();
    auto d      = d_t->arg();
    auto ls     = Lit::isa(s);
    auto ld     = Lit::isa(d);

    if (s_t == d_t) return x;
    if (x->isa<Bot>()) return world.bot(d_t);

    constexpr bool sf = id == conv::f2f || id == conv::f2s || id == conv::f2u;
    constexpr bool df = id == conv::f2f || id == conv::s2f || id == conv::u2f;

    auto sw = sf ? isa_f(s_t) : (ls ? Idx::size2bitwidth(*ls) : std::optional<nat_t>());
    auto dw = df ? isa_f(d_t) : (ld ? Idx::size2bitwidth(*ld) : std::optional<nat_t>());

    if (auto l = Lit::isa(x); l && sw && dw)
        if (auto res = fold_conv<id>(*sw, *dw, *l)) return world.lit(d_t, *res);

    return {};
}

const Def* normalize_abs(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<abs>(world, type, arg)) return lit;
    return {};
}

template<round id>
const Def* normalize_round(const Def* type, const Def*, const Def* arg) {
    auto& world = type->world();
    if (auto lit = fold<round, id>(world, type, arg)) return lit;
    return {};
}

MIM_math_NORMALIZER_IMPL

} // namespace mim::plug::math
