#pragma once

#include <absl/container/inlined_vector.h>

#include "mim/util/span.h"

namespace mim {

/// Use up to 4 words (i.e., 4 * sizeof(size_t)) of inlined storage, rounded up.
template<class T>
static constexpr size_t Default_Inlined_Size = std::max((size_t)1, 4 * sizeof(size_t) / sizeof(T));

/// This is a thin wrapper for
/// [`absl::InlinedVector<T, N, A>`](https://github.com/abseil/abseil-cpp/blob/master/absl/container/inlined_vector.h)
/// which is a drop-in replacement for [`std::vector<T, A>`](https://en.cppreference.com/w/cpp/container/vector).
/// In addition, there are generator-like/lambda-based constructors and conversions to Span available.
template<class T, size_t N = Default_Inlined_Size<T>, class A = std::allocator<T>>
class Vector : public absl::InlinedVector<T, N, A> {
public:
    using Base = absl::InlinedVector<T, N, A>;

    /// @name Constructors
    ///@{
    using Base::Base;
    template<class F>
    constexpr explicit Vector(size_t size, F&& f) noexcept(std::is_nothrow_invocable_r_v<T, F, size_t>
                                                           && std::is_nothrow_assignable_v<T&, T>)
        requires(std::is_invocable_r_v<T, F, size_t>)
        : Base(size) {
        for (size_t i = 0; i != size; ++i)
            (*this)[i] = std::invoke(f, i);
    }

    template<std::ranges::forward_range R, class F>
    constexpr explicit Vector(R&& range,
                              F&& f) noexcept(std::is_nothrow_invocable_r_v<T, F, decltype(*std::ranges::begin(range))>
                                              && std::is_nothrow_assignable_v<T&, T>)
        requires(std::is_invocable_r_v<T, F, decltype(*std::ranges::begin(range))>
                 && !std::is_same_v<std::decay_t<R>, Vector>)
        : Base(std::ranges::distance(range)) {
        auto ri = std::ranges::begin(range);
        for (auto& elem : *this)
            elem = std::invoke(f, *ri++);
    }
    ///@}

    /// @name insert_range / append_range
    /// Available in C++23 but not absl.
    ///@{
    template<std::ranges::forward_range R>
    constexpr void insert_range(Base::const_iterator pos, R&& r) {
        Base::insert(pos, r.begin(), r.end());
    }
    template<std::ranges::forward_range R>
    constexpr void append_range(R&& r) {
        insert_range(Base::end(), r);
    }
    ///@}

    /// @name Span
    ///@{
    constexpr auto span() noexcept { return Span{Base::data(), Base::size()}; }
    constexpr auto span() const noexcept { return Span{Base::data(), Base::size()}; }
    constexpr auto view() const noexcept { return span(); }
    ///@}

    friend void swap(Vector& v1, Vector& v2) noexcept(noexcept(v1.swap(v2))) { v1.swap(v2); }
};

static_assert(std::ranges::contiguous_range<Vector<int>>);

/// @name Deduction Guides
///@{
template<class I, class A = std::allocator<typename std::iterator_traits<I>::value_type>>
Vector(I, I, A = A()) -> Vector<typename std::iterator_traits<I>::value_type,
                                Default_Inlined_Size<typename std::iterator_traits<I>::value_type>,
                                A>;
///@}

/// @name erase
///@{
template<class T, size_t N, class A, class U>
typename Vector<T, N, A>::size_type erase(Vector<T, N, A>& c, const U& value) noexcept {
    auto it = std::remove(c.begin(), c.end(), value);
    auto r  = c.end() - it;
    c.erase(it, c.end());
    return r;
}

template<class T, size_t N, class A, class Pred>
typename Vector<T, N, A>::size_type erase_if(Vector<T, N, A>& c, Pred pred) noexcept {
    auto it = std::remove_if(c.begin(), c.end(), pred);
    auto r  = c.end() - it;
    c.erase(it, c.end());
    return r;
}
///@}

} // namespace mim
