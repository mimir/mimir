#pragma once

#include <functional>
#include <iostream>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string>

#include <fe/assert.h>

namespace mim {
namespace detail {

/// Does @p T provide an ostream operator?
template<class T>
concept Streamable = requires(std::ostream& os, T a) { os << a; };

/// Does @p has a @p range and a function @p f to invoke?
template<class T>
concept Elemable = requires(T elem) {
    elem.range;
    elem.f;
};

template<class R, class F>
std::ostream& range(std::ostream& os, const R& r, F f, const char* sep = ", ") {
    for (const char* curr_sep = ""; const auto& elem : r) {
        for (auto i = curr_sep; *i != '\0'; ++i)
            os << *i;

        if constexpr (std::is_invocable_v<F, std::ostream&, decltype(elem)>)
            std::invoke(f, os, elem);
        else
            std::invoke(f, elem);
        curr_sep = sep;
    }
    return os;
}

bool match2nd(std::ostream& os, const char* next, const char*& s, const char c);
} // namespace detail

/// @name Formatted Output
/// @anchor fmt
/// Provides a `printf`-like interface to format @p s with @p args and puts it into @p os.
/// Use `{}` as a placeholder within your format string @p s.
/// * By default, `os << t` will be used to stream the appropriate argument:
/// ```
/// print(os, "int: {}, float: {}", 23, 42.f);
/// ```
/// * You can also place a function as argument to inject arbitrary code:
/// ```
/// print(os, "int: {}, fun: {}, float: {}", 23, [&]() { os << "hey :)"}, 42.f);
/// ```
/// * There is also a variant to pass @p os to the function, if you don't have easy access to it:
/// ```
/// print(os, "int: {}, fun: {}, float: {}", 23, [&](auto& os) { os << "hey :)"}, 42.f);
/// ```
/// * You can put a `std::ranges::range` as argument.
/// This will output a list - using the given specifier as separator.
/// Each element `elem` of the range will be output via `os << elem`:
/// ```
/// std::vector<int> v = {0, 1, 2, 3};
/// print(os, "v: {, }", v);
/// ```
/// * If you have a `std::ranges::range` that needs to be formatted in a more complicated way, bundle it with an Elem:
/// ```
/// std::vector<int> v = {3, 2, 1, 0};
/// size_t i = 0;
/// print(os, "v: {, }", Elem(v, [&](auto elem) { print(os, "{}: {}", i++, elem); }));
/// ```
/// * You again have the option to pass @p os to the bundled function:
/// ```
/// std::vector<int> v = {3, 2, 1, 0};
/// size_t i = 0;
/// print(os, "v: {, }", Elem(v, [&](auto& os, auto elem) { print(os, "{}: {}", i++, elem); }));
/// * Use `{{` or `}}` to literally output `{` or `{`:
/// print(os, "{{{}}}", 23); // "{23}"
/// ```
/// @see Tab
///@{

/// Use with print to output complicated `std::ranges::range`s.
template<class R, class F>
struct Elem {
    Elem(const R& range, const F& f)
        : range(range)
        , f(f) {}

    const R range;
    const F f;
};

/// Wrap all elements in @p range through `os << T(elem)`.
template<class T>
auto elems(std::ostream& os, const auto& range) {
    return Elem(range, [&os](const auto& elem) { os << T(elem); });
}

/// Create function-wrapper objects amenable for `opeartor<<`.
template<class F>
struct StreamFn {
    F f;
    friend std::ostream& operator<<(std::ostream& os, const StreamFn& s) { return s.f(os); }
};

template<class F>
StreamFn(F) -> StreamFn<F>;

std::ostream& print(std::ostream& os, const char* s); ///< Base case.

template<class T, class... Args>
std::ostream& print(std::ostream& os, const char* s, T&& t, Args&&... args) {
    while (*s != '\0') {
        auto next = s + 1;

        switch (*s) {
            case '{': {
                if (detail::match2nd(os, next, s, '{')) continue;
                s++; // skip opening brace '{'

                std::string spec;
                while (*s != '\0' && *s != '}')
                    spec.push_back(*s++);
                assert(*s == '}' && "unmatched closing brace '}' in format string");

                if constexpr (std::is_invocable_v<decltype(t)>) {
                    std::invoke(t);
                } else if constexpr (std::is_invocable_v<decltype(t), std::ostream&>) {
                    std::invoke(t, os);
                } else if constexpr (detail::Streamable<decltype(t)>) {
                    auto flags = std::ios_base::fmtflags(os.flags());

                    if (spec == "b")
                        assert(false && "TODO");
                    else if (spec == "o")
                        os << std::oct;
                    else if (spec == "d")
                        os << std::dec;
                    else if (spec == "x")
                        os << std::hex;

                    os << t;
                    os.flags(flags);
                } else if constexpr (detail::Elemable<decltype(t)>) {
                    detail::range(os, t.range, t.f, spec.c_str());
                } else if constexpr (std::ranges::range<decltype(t)>) {
                    detail::range(os, t, [&](const auto& x) { os << x; }, spec.c_str());
                } else {
                    static_assert(false, "cannot print T t");
                }

                ++s; // skip closing brace '}'
                // call even when *s == '\0' to detect extra arguments
                return print(os, s, std::forward<Args>(args)...);
            }
            case '}':
                if (detail::match2nd(os, next, s, '}')) continue;
                assert(false && "unmatched/unescaped closing brace '}' in format string");
                fe::unreachable();
            default: os << *s++;
        }
    }

    assert(false && "invalid format string for 's'");
    fe::unreachable();
}

/// As above but end with `std::endl`.
template<class... Args>
std::ostream& println(std::ostream& os, const char* fmt, Args&&... args) {
    return print(os, fmt, std::forward<Args>(args)...) << std::endl;
}

/// Wraps mim::print to output a formatted `std:string`.
template<class... Args>
std::string fmt(const char* s, Args&&... args) {
    std::ostringstream os;
    print(os, s, std::forward<Args>(args)...);
    return os.str();
}

/// Wraps mim::print to throw `T` with a formatted message.
template<class T = std::logic_error, class... Args>
[[noreturn]] void error(const char* fmt, Args&&... args) {
    std::ostringstream oss;
    print(oss << "error: ", fmt, std::forward<Args>(args)...);
    throw T(oss.str());
}

#ifdef NDEBUG
#    define assertf(condition, ...)  \
        do {                         \
            (void)sizeof(condition); \
        } while (false)
#else
#    define assertf(condition, ...)                                  \
        do {                                                         \
            if (!(condition)) {                                      \
                mim::errf("{}:{}: assertion: ", __FILE__, __LINE__); \
                mim::errln(__VA_ARGS__);                             \
                fe::breakpoint();                                    \
            }                                                        \
        } while (false)
#endif
///@}

/// @name out/err
/// mim::print%s to `std::cout`/`std::cerr`; the *`ln` variants emit an additional `std::endl`.
///@{
// clang-format off
template<class... Args> std::ostream& outf (const char* fmt, Args&&... args) { return print(std::cout, fmt, std::forward<Args>(args)...); }
template<class... Args> std::ostream& errf (const char* fmt, Args&&... args) { return print(std::cerr, fmt, std::forward<Args>(args)...); }
template<class... Args> std::ostream& outln(const char* fmt, Args&&... args) { return outf(fmt, std::forward<Args>(args)...) << std::endl; }
template<class... Args> std::ostream& errln(const char* fmt, Args&&... args) { return errf(fmt, std::forward<Args>(args)...) << std::endl; }
// clang-format on
///@}

/// Keeps track of indentation level.
class Tab {
public:
    Tab(std::string_view tab = {"    "}, size_t indent = 0)
        : tab_(tab)
        , indent_(indent) {}

    /// @name Getters
    ///@{
    size_t indent() const { return indent_; }
    std::string_view tab() const { return tab_; }
    ///@}

    /// @name print
    /// Wraps mim::print to prefix it with indentation.
    /// @see @ref fmt "Formatted Output"
    ///@{
    template<class... Args>
    std::ostream& print(std::ostream& os, const char* s, Args&&... args) {
        for (size_t i = 0; i < indent_; ++i)
            os << tab_;
        return mim::print(os, s, std::forward<Args>(args)...);
    }
    /// Same as Tab::print but **prepends** a `std::endl` to @p os.
    template<class... Args>
    std::ostream& lnprint(std::ostream& os, const char* s, Args&&... args) {
        return print(os << std::endl, s, std::forward<Args>(args)...);
    }
    /// Same as Tab::print but **appends** a `std::endl` to @p os.
    template<class... Args>
    std::ostream& println(std::ostream& os, const char* s, Args&&... args) {
        return print(os, s, std::forward<Args>(args)...) << std::endl;
    }
    ///@}

    // clang-format off
    /// @name Creates a new Tab
    ///@{
    [[nodiscard]] Tab operator++(int) {                      return {tab_, indent_++}; }
    [[nodiscard]] Tab operator--(int) { assert(indent_ > 0); return {tab_, indent_--}; }
    [[nodiscard]] Tab operator+(size_t indent) const {                      return {tab_, indent_ + indent}; }
    [[nodiscard]] Tab operator-(size_t indent) const { assert(indent_ > 0); return {tab_, indent_ - indent}; }
    ///@}

    /// @name Modifies this Tab
    ///@{
    Tab& operator++() {                      ++indent_; return *this; }
    Tab& operator--() { assert(indent_ > 0); --indent_; return *this; }
    Tab& operator+=(size_t indent) {                      indent_ += indent; return *this; }
    Tab& operator-=(size_t indent) { assert(indent_ > 0); indent_ -= indent; return *this; }
    Tab& operator=(size_t indent) { indent_ = indent; return *this; }
    Tab& operator=(std::string_view tab) { tab_ = tab; return *this; }
    ///@}
    // clang-format on

private:
    std::string_view tab_;
    size_t indent_ = 0;
};

} // namespace mim
