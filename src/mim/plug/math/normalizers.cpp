#include <mim/normalize.h>

#include "mim/plug/math/math.h"

namespace mim::plug::math {

namespace {

// clang-format off
template<class Id, Id id, nat_t w>
Res fold(u64 a) {
    using T = w2f<w>;
    auto x = bitcast<T>(a);
    if constexpr (std::is_same_v<Id, tri>) {
        if constexpr (false) {}
        else if constexpr (id == tri:: sin ) return  sin (x);
        else if constexpr (id == tri:: cos ) return  cos (x);
        else if constexpr (id == tri:: tan ) return  tan (x);
        else if constexpr (id == tri:: sinh) return  sinh(x);
        else if constexpr (id == tri:: cosh) return  cosh(x);
        else if constexpr (id == tri:: tanh) return  tanh(x);
        else if constexpr (id == tri::asin ) return asin (x);
        else if constexpr (id == tri::acos ) return acos (x);
        else if constexpr (id == tri::atan ) return atan (x);
        else if constexpr (id == tri::asinh) return asinh(x);
        else if constexpr (id == tri::acosh) return acosh(x);
        else if constexpr (id == tri::atanh) return atanh(x);
        else fe::unreachable();
    } else if constexpr (std::is_same_v<Id, rt>) {
        if constexpr (false) {}
        else if constexpr (id == rt::sq) return std::sqrt(x);
        else if constexpr (id == rt::cb) return std::cbrt(x);
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, exp>) {
        if constexpr (false) {}
        else if constexpr (id == exp::exp  ) return std::exp  (x);
        else if constexpr (id == exp::exp2 ) return std::exp2 (x);
        else if constexpr (id == exp::exp10) return std::pow(T(10), x);
        else if constexpr (id == exp::log  ) return std::log  (x);
        else if constexpr (id == exp::log2 ) return std::log2 (x);
        else if constexpr (id == exp::log10) return std::log10(x);
        else fe::unreachable();
    } else if constexpr (std::is_same_v<Id, er>) {
        if constexpr (false) {}
        else if constexpr (id == er::f ) return std::erf (x);
        else if constexpr (id == er::fc) return std::erfc(x);
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, gamma>) {
        if constexpr (false) {}
        else if constexpr (id == gamma::t) return std::tgamma(x);
        else if constexpr (id == gamma::l) return std::lgamma(x);
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, round>) {
        if constexpr (false) {}
        else if constexpr (id == round::f) return  std::floor (x);
        else if constexpr (id == round::c) return  std::ceil (x);
        else if constexpr (id == round::r) return  std::round (x);
        else if constexpr (id == round::t) return  std::trunc (x);
        else static_assert(false, "missing sub tag");
    } else {
        static_assert(false, "missing tag");
    }
}

template<class Id, nat_t w>
Res fold(u64 a) {
    using T = w2f<w>;
    auto x = bitcast<T>(a);
    if constexpr (std::is_same_v<Id, abs>)
        return std::abs(x);
    else
        static_assert(false, "missing tag");
}

template<class Id>
const Def* fold(World& world, const Def* type, const Def* a) {
    if (a->isa<Bot>()) return world.bot(type);
    auto la = a->isa<Lit>();

    if (la) {
        nat_t width = *isa_f(a->type());
        Res res;
        switch (width) {
#define CODE(i) \
    case i: res = fold<Id, i>(la->get()); break;
            MIM_F16_32_64(CODE)
#undef CODE
            default: return nullptr;
        }

        return res ? world.lit(type, *res) : nullptr;
    }

    return nullptr;
}

template<class Id, Id id, nat_t w>
Res fold(u64 a, u64 b) {
    using T = w2f<w>;
    auto x = bitcast<T>(a), y = bitcast<T>(b);
    if constexpr (std::is_same_v<Id, arith>) {
        if constexpr (false) {}
        else if constexpr (id == arith::add) return     x + y;
        else if constexpr (id == arith::sub) return     x - y;
        else if constexpr (id == arith::mul) return     x * y;
        else if constexpr (id == arith::div) return     x / y;
        else if constexpr (id == arith::rem) return rem(x,  y);
        else static_assert(false, "missing sub tag");
    } else if constexpr (std::is_same_v<Id, math::extrema>) {
        if (x == T(-0.0) && y == T(+0.0)) return (id == extrema::fmin || id == extrema::ieee754min) ? x : y;
        if (x == T(+0.0) && y == T(-0.0)) return (id == extrema::fmin || id == extrema::ieee754min) ? y : x;

        if constexpr (id == extrema::fmin || id == extrema::fmax) {
            return id == extrema::fmin ? std::fmin(x, y) : std::fmax(x, y);
        } else if constexpr (id == extrema::ieee754min || id == extrema::ieee754max) {
            if (std::isnan(x)) return x;
            if (std::isnan(y)) return y;
            return id == extrema::ieee754min ? std::fmin(x, y) : std::fmax(x, y);
        } else {
            static_assert(false, "missing sub tag");
        }
    } else if constexpr (std::is_same_v<Id, pow>) {
        return std::pow(a, b);
    } else if constexpr (std::is_same_v<Id, cmp>) {
        using std::isunordered;
        bool res = false;
        res |= ((id & cmp::u) != cmp::f) && isunordered(x, y);
        res |= ((id & cmp::g) != cmp::f) && x > y;
        res |= ((id & cmp::l) != cmp::f) && x < y;
        res |= ((id & cmp::e) != cmp::f) && x == y;
        return res;
    } else {
        static_assert(false, "missing tag");
    }
}
// clang-format on

template<class Id, Id id>
const Def* fold(World& world, const Def* type, const Def* a) {
    if (a->isa<Bot>()) return world.bot(type);

    if (auto la = Lit::isa(a)) {
        nat_t width = *isa_f(a->type());
        Res res;
        switch (width) {
#define CODE(i) \
    case i: res = fold<Id, id, i>(*la); break;
            MIM_F16_32_64(CODE)
#undef CODE
            default: return nullptr;
        }

        return res ? world.lit(type, *res) : nullptr;
    }

    return nullptr;
}

// Note that @p a and @p b are passed by reference as fold also commutes if possible.
template<class Id, Id id>
const Def* fold(World& world, const Def* type, const Def*& a, const Def*& b) {
    if (a->isa<Bot>() || b->isa<Bot>()) return world.bot(type);

    if (auto la = Lit::isa(a)) {
        if (auto lb = Lit::isa(b)) {
            nat_t width = *isa_f(a->type());
            Res res;
            switch (width) {
#define CODE(i) \
    case i: res = fold<Id, id, i>(*la, *lb); break;
                MIM_F16_32_64(CODE)
#undef CODE
                default: return nullptr;
            }

            return res ? world.lit(type, *res) : nullptr;
        }
    }

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

template<class Id, Id id, nat_t sw, nat_t dw>
Res fold(u64 a) {
    using S = std::conditional_t<id == conv::s2f, w2s<sw>, std::conditional_t<id == conv::u2f, w2u<sw>, w2f<sw>>>;
    using D = std::conditional_t<id == conv::f2s, w2s<dw>, std::conditional_t<id == conv::f2u, w2u<dw>, w2f<dw>>>;
    if constexpr (std::is_void_v<S> || std::is_void_v<D>)
        return {};
    else
        return D(bitcast<S>(a));
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

    if (auto l = Lit::isa(x); l && ls && ld) {
        constexpr bool sf     = id == conv::f2f || id == conv::f2s || id == conv::f2u;
        constexpr bool df     = id == conv::f2f || id == conv::s2f || id == conv::u2f;
        constexpr nat_t min_s = sf ? 16 : 1;
        constexpr nat_t min_d = df ? 16 : 1;
        auto sw               = sf ? isa_f(s_t) : Idx::size2bitwidth(*ls);
        auto dw               = df ? isa_f(d_t) : Idx::size2bitwidth(*ld);

        if (sw && dw) {
            Res res;
            // clang-format off
            if (false) {}
#define M(S, D)                                         \
            else if (S == *sw && D == *dw) {            \
                if constexpr (S >= min_s && D >= min_d) \
                    res = fold<conv, id, S, D>(*l);     \
                else                                    \
                    return {};                          \
            }
            M( 1,  1) M( 1,  8) M( 1, 16) M( 1, 32) M( 1, 64)
            M( 8,  1) M( 8,  8) M( 8, 16) M( 8, 32) M( 8, 64)
            M(16,  1) M(16,  8) M(16, 16) M(16, 32) M(16, 64)
            M(32,  1) M(32,  8) M(32, 16) M(32, 32) M(32, 64)
            M(64,  1) M(64,  8) M(64, 16) M(64, 32) M(64, 64)

            else fe::unreachable();
            // clang-format on
            return world.lit(d_t, *res);
        }
    }

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
