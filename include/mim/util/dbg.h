#pragma once

#include <sstream>
#include <stdexcept>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fe/loc.h>
#include <fe/sym.h>
#include <fe/term.h>

namespace mim {

/// Wraps `std::format` to throw `T` with a formatted message.
template<class T = std::logic_error, class... Args>
[[noreturn]] void error(std::format_string<Args...> fmt, Args&&... args) {
    throw T("error: " + std::format(fmt, std::forward<Args>(args)...));
}

using fe::Loc;
using fe::Pos;
using fe::Sym;

class Error : public std::exception {
public:
    enum class Tag {
        Error,
        Warn,
        Note,
    };

    struct Msg {
        Loc loc;
        Tag tag;
        std::string str;

        friend std::ostream& operator<<(std::ostream&, const Msg&);
    };

    /// @name Constructors
    ///@{
    Error()             = default;
    Error(const Error&) = default;
    Error(Error&& other)
        : Error() {
        swap(*this, other);
    }
    /// Creates a single Tag::Error message.
    Error(Loc loc, const std::string& str)
        : msgs_{
              {loc, Tag::Error, str}
    } {}
    ///@}

    /// @name Getters
    ///@{
    const auto& msgs() const { return msgs_; }
    size_t num_msgs() const {
        assert(num_errors() + num_warnings() + num_notes() == msgs_.size());
        return msgs_.size();
    }
    size_t num_errors() const { return num_errors_; }
    size_t num_warnings() const { return num_warnings_; }
    size_t num_notes() const { return num_notes_; }
    ///@}

    /// @name Add formatted message
    ///@{
    template<class... Args>
    Error& msg(Loc loc, Tag tag, std::format_string<Args...> s, Args&&... args) {
        msgs_.emplace_back(loc, tag, std::format(s, std::forward<Args>(args)...));
        return *this;
    }

    // clang-format off
    template<class... Args> Error& error(Loc loc, std::format_string<Args...> s, Args&&... args) { ++num_errors_;   return msg(loc, Tag::Error, s, std::forward<Args>(args)...); }
    template<class... Args> Error& warn (Loc loc, std::format_string<Args...> s, Args&&... args) { ++num_warnings_; return msg(loc, Tag::Warn,  s, std::forward<Args>(args)...); }
    template<class... Args> Error& note (Loc loc, std::format_string<Args...> s, Args&&... args) {
        assert(num_errors() > 0 || num_warnings() > 0); /*                                    */   ++num_notes_;    return msg(loc, Tag::Note,  s, std::forward<Args>(args)...); }
    // clang-format on
    ///@}

    /// @name Handle Errors/Warnings
    ///@{
    void clear();
    /// If errors occurred, claim them and throw; if warnings occurred, claim them and report to @p os.
    void ack(std::ostream& os = std::cerr);
    ///@}

    const char* what() const noexcept override {
        if (what_.empty()) {
            std::ostringstream oss;
            oss << *this;
            what_ = oss.str();
        }
        return what_.c_str();
    }

    friend std::ostream& operator<<(std::ostream& o, Tag tag) {
        // clang-format off
        switch (tag) {
            case Tag::Error: return o << fe::term::FG::Red     << "error";
            case Tag::Warn:  return o << fe::term::FG::Magenta << "warning";
            case Tag::Note:  return o << fe::term::FG::Green   << "note";
            default: fe::unreachable();
        }
        // clang-format on
    }

    friend std::ostream& operator<<(std::ostream& os, const Error& e) {
        for (const auto& msg : e.msgs())
            os << msg << std::endl;
        return os;
    }

    friend void swap(Error& e1, Error& e2) noexcept {
        using std::swap;
        // clang-format off
        swap(e1.msgs_,         e2.msgs_);
        swap(e1.num_errors_,   e2.num_errors_);
        swap(e1.num_warnings_, e2.num_warnings_);
        swap(e1.num_notes_,    e2.num_notes_);
        // clang-format on
    }

private:
    std::vector<Msg> msgs_;
    size_t num_errors_   = 0;
    size_t num_warnings_ = 0;
    size_t num_notes_    = 0;
    mutable std::string what_;
};

/// @name Formatted Output
///@{
/// Single Error that `throw`s immediately.
template<class... Args>
[[noreturn]] void error(Loc loc, std::format_string<Args...> f, Args&&... args) {
    throw Error(loc, std::format(f, std::forward<Args>(args)...));
}
///@}

struct Dbg {
public:
    /// @name Constructors
    ///@{
    constexpr Dbg() noexcept           = default;
    constexpr Dbg(const Dbg&) noexcept = default;
    constexpr Dbg(Loc loc, Sym sym) noexcept
        : loc_(loc)
        , sym_(sym) {}
    constexpr Dbg(Loc loc) noexcept
        : Dbg(loc, {}) {}
    constexpr Dbg(Sym sym) noexcept
        : Dbg({}, sym) {}
    Dbg& operator=(const Dbg&) noexcept = default;
    ///@}

    /// @name Getters
    ///@{
    Sym sym() const { return sym_; }
    Loc loc() const { return loc_; }
    bool is_anon() const { return !sym() || sym() == '_'; }
    explicit operator bool() const { return sym().operator bool(); }
    ///@}

    /// @name Setters
    ///@{
    Dbg& set(Sym sym) { return sym_ = sym, *this; }
    Dbg& set(Loc loc) { return loc_ = loc, *this; }
    ///@}

private:
    Loc loc_;
    Sym sym_;

    friend std::ostream& operator<<(std::ostream& os, const Dbg& dbg) { return os << dbg.sym(); }
};

} // namespace mim

#ifndef DOXYGEN // clang-format off
template<> struct std::formatter<mim::Dbg       > : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error     > : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error::Tag> : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error::Msg> : fe::ostream_formatter {};
#endif // clang-format on

namespace mim {

inline std::ostream& operator<<(std::ostream& os, const Error::Msg& msg) {
    return os << std::format("{}{}: {}: {}{}", fe::term::FG::Yellow, msg.loc, msg.tag, fe::term::FG::Reset, msg.str);
}

} // namespace mim
