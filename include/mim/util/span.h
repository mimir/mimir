#pragma once

#include <span>

namespace mim {

/// Something which behaves like `std::vector` or `std::array`.
template<class Vec>
concept Vectorlike = requires(Vec vec) {
    typename Vec::value_type;
    vec.size();
    vec.data();
};

/// This is a thin wrapper for [`std::span<T, N>`](https://en.cppreference.com/w/cpp/container/span)
/// with the following additional features:
/// * Constructor with `std::initializer_list` (C++26 will get this ...)
/// * Constructor for any compatible Vectorlike argument
/// * rsubspan (reverse subspan)
/// * structured binding, if `N != std::dynamic_extent:`
/// ```
/// void f(Span<int, 3> span) {
///     auto& [a, b, c] = span;
///     b = 23;
///     // ...
/// }
/// ```
template<class T, size_t N = std::dynamic_extent>
class Span : public std::span<T, N> {
public:
    using Base              = std::span<T, N>;
    constexpr static auto D = std::dynamic_extent;

    using Base::data;
    using Base::empty;
    using Base::size;

    /// @name Constructors
    ///@{
    using Base::Base;
    explicit(N != D) constexpr Span(std::initializer_list<T> init) noexcept
        : Base(std::begin(init), std::ranges::distance(init)) {}
    constexpr Span(std::span<T, N> span) noexcept
        : Base(span) {}
    template<Vectorlike Vec>
    requires(std::is_same_v<typename Vec::value_type, T>)
    explicit(N != D) constexpr Span(Vec& vec) noexcept(noexcept(vec.data()) && noexcept(vec.size()))
        : Base(vec.data(), vec.size()) {}
    template<Vectorlike Vec>
    requires(std::is_same_v<std::add_const_t<typename Vec::value_type>, std::add_const_t<T>>)
    explicit(N != D) constexpr Span(const Vec& vec) noexcept(noexcept(vec.data()) && noexcept(vec.size()))
        : Base(const_cast<T*>(vec.data()), vec.size()) {}
    constexpr explicit Span(typename Base::pointer p) noexcept
        : Base(p, N) {
        static_assert(N != D);
    }
    ///@}

    /// @name subspan
    /// Wrappers for `std::span::subspan` that return a
    /// `mim::Span`. Example: If `span` points to `0, 1, 2, 3, 4, 5, 6, 7, 8, 9`, then
    /// * `span.subspan<2, 5>()` and `span.subspan(2, 5)` will point to `2, 3, 4, 5, 6`.
    /// * `span.subspan<2>()` and `span.subspan(2)` will point to `2, 3, 4, 5, 6, 7, 8, 9`.
    ///@{
    [[nodiscard]] constexpr Span<T, D> subspan(size_t i, size_t n = D) const noexcept { return Base::subspan(i, n); }

    template<size_t i, size_t n = D>
    [[nodiscard]] constexpr Span<T, n != D ? n : (N != D ? N - i : D)> subspan() const noexcept {
        return Base::template subspan<i, n>();
    }

    /// Get first `n` elements while keeping track of size statically - useful for structured binding!
    template<size_t n>
    [[nodiscard]] constexpr Span<T, n> span() const noexcept {
        return Base::template subspan<0, n>();
    }
    ///@}

    /// @name rsubspan
    /// Similar to Span::subspan but in *reverse*:
    ///@{
    [[nodiscard]] constexpr Span<T, D> rsubspan(size_t i, size_t n = D) const noexcept {
        return n != D ? subspan(size() - i - n, n) : subspan(0, size() - i);
    }

    /// `span.rsubspan(3, 5)` removes the last 3 elements and picks 5 elements starting before those.
    template<size_t i, size_t n = D>
    [[nodiscard]] constexpr Span<T, n != D ? n : (N != D ? N - i : D)> rsubspan() const noexcept {
        if constexpr (n != D)
            return Span<T, n>(data() + size() - i - n);
        else if constexpr (N != D)
            return Span<T, N - i>(data());
        else
            return Span<T, D>(data(), size() - i);
    }
    ///@}

    [[nodiscard]] constexpr operator bool() const noexcept { return !empty(); } ///< Is not empty?
};

static_assert(std::ranges::contiguous_range<Span<int>>);

template<class T, size_t N = std::dynamic_extent>
using View = Span<const T, N>;

/// @name Deduction Guides
///@{
// clang-format off
template<class I, class E>  Span(I, E)                    -> Span<std::remove_reference_t<std::iter_reference_t<I>>>;
template<class T, size_t N> Span(T (&)[N])                -> Span<      T, N>;
template<class T, size_t N> Span(      std::array<T, N>&) -> Span<      T, N>;
template<class T, size_t N> Span(const std::array<T, N>&) -> Span<const T, N>;
template<class R>           Span(R&&)                     -> Span<std::remove_reference_t<std::ranges::range_reference_t<R>>>;
template<Vectorlike Vec>    Span(      Vec&)              -> Span<      typename Vec::value_type, std::dynamic_extent>;
template<Vectorlike Vec>    Span(const Vec&)              -> Span<const typename Vec::value_type, std::dynamic_extent>;
// clang-format on
///@}

template<size_t I, class T, size_t N>
requires(N != std::dynamic_extent)
constexpr decltype(auto) get(Span<T, N> span) noexcept {
    static_assert(I < N, "index I out of bound N");
    return span[I];
}

} // namespace mim

namespace std {
/// @name Structured Binding Support for Span
///@{
template<class T, size_t N>
requires(N != std::dynamic_extent)
struct tuple_size<mim::Span<T, N>> : std::integral_constant<size_t, N> {};

template<size_t I, class T, size_t N>
requires(N != std::dynamic_extent)
struct tuple_element<I, mim::Span<T, N>> {
    using type = typename mim::Span<T, N>::reference;
};
///@}
} // namespace std
