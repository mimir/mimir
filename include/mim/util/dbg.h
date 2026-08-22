#pragma once

#include <algorithm>
#include <sstream>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fe/assert.h>
#include <fe/loc.h>
#include <fe/sym.h>
#include <fe/term.h>

namespace mim {

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
    Error() = default;
    /// Creates a single Tag::Error message.
    Error(Loc loc, const std::string& str)
        : msgs_{
              {loc, Tag::Error, str}
    } {}
    ///@}

    /// @name Getters
    ///@{
    const auto& msgs() const { return msgs_; }
    size_t num_msgs() const { return msgs_.size(); }
    size_t num_errors() const { return std::ranges::count(msgs_, Tag::Error, &Msg::tag); }
    size_t num_warnings() const { return std::ranges::count(msgs_, Tag::Warn, &Msg::tag); }
    size_t num_notes() const { return std::ranges::count(msgs_, Tag::Note, &Msg::tag); }
    ///@}

    /// @name Add formatted message
    ///@{
    template<class... Args>
    Error& msg(Loc loc, Tag tag, std::format_string<Args...> s, Args&&... args) {
        msgs_.emplace_back(loc, tag, std::format(s, std::forward<Args>(args)...));
        return *this;
    }

    // clang-format off
    template<class... Args> Error& error(Loc loc, std::format_string<Args...> s, Args&&... args) { return msg(loc, Tag::Error, s, std::forward<Args>(args)...); }
    template<class... Args> Error& warn (Loc loc, std::format_string<Args...> s, Args&&... args) { return msg(loc, Tag::Warn,  s, std::forward<Args>(args)...); }
    template<class... Args> Error& note (Loc loc, std::format_string<Args...> s, Args&&... args) {
        assert(num_errors() > 0 || num_warnings() > 0);
        return msg(loc, Tag::Note, s, std::forward<Args>(args)...);
    }
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

private:
    std::vector<Msg> msgs_;
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

    /// @name Comparison and Hashing
    /// Dbg%s are interned in the Driver so that a Def only has to store a `u32` index; see Def::dbg_.
    ///@{
    /// @note Like Loc::operator==, this only compares Loc::path by pointer identity.
    bool operator==(const Dbg& other) const noexcept { return loc_ == other.loc_ && sym_ == other.sym_; }

    template<class H>
    friend H AbslHashValue(H h, Dbg dbg) noexcept {
        return H::combine(std::move(h), dbg.loc_.path, dbg.loc_.begin.row, dbg.loc_.begin.col, dbg.loc_.finis.row,
                          dbg.loc_.finis.col, dbg.sym_);
    }
    ///@}

private:
    Loc loc_;
    Sym sym_;

    friend std::ostream& operator<<(std::ostream& os, const Dbg& dbg) { return os << dbg.sym(); }
};

/// Opaque handle to an interned Dbg (see Def::dbg_).
/// Handing one Def another's key copies the interned index verbatim: no Dbg is materialised and nothing is
/// looked up in the Driver's table. @warning Keys are only meaningful within the Driver that interned them.
class DbgKey {
public:
    constexpr DbgKey() noexcept = default; ///< The empty Dbg.

private:
    constexpr explicit DbgKey(uint32_t key) noexcept
        : key_(key) {}

    uint32_t key_ = 0;

    friend class Def;
    friend class World; ///< World::set_loc pre-interns World::get_loc into a key.
};

} // namespace mim

#ifndef DOXYGEN // clang-format off
template<> struct std::formatter<mim::Dbg       > : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error     > : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error::Tag> : fe::ostream_formatter {};
template<> struct std::formatter<mim::Error::Msg> : fe::ostream_formatter {};
#endif // clang-format on

namespace mim {

/// Streams @p str, rendering the `` `code` `` spans our diagnostics cite in color.
/// Color and backticks are alternative ways of setting a citation apart, so the backticks are dropped
/// when coloring and kept verbatim otherwise.
inline std::ostream& stream_code(std::ostream& os, std::string_view str) {
    if (!fe::term::use_color(os)) return os << str;

    for (size_t i = 0, e = str.size(); i != e;) {
        auto l = str.find('`', i);
        if (l == std::string_view::npos) return os << str.substr(i);
        auto r = str.find('`', l + 1);
        if (r == std::string_view::npos) return os << str.substr(i); // unpaired: not a citation
        os << str.substr(i, l - i) << fe::term::FG::Cyan << str.substr(l + 1, r - l - 1) << fe::term::FG::Reset;
        i = r + 1;
    }
    return os;
}

// Streamed piecewise instead of via std::format: a std::formatter cannot see its destination stream,
// so embedded fe::term::FG values would resolve Mode::Auto to "no color"; see fe/term.h.
inline std::ostream& operator<<(std::ostream& os, const Error::Msg& msg) {
    os << fe::term::FG::Yellow << msg.loc << ": " << msg.tag << ": " << fe::term::FG::Reset;
    return stream_code(os, msg.str);
}

} // namespace mim
