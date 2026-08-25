#pragma once

#include <algorithm>
#include <memory>
#include <sstream>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fe/assert.h>
#include <fe/format.h>
#include <fe/loc.h>
#include <fe/src.h>
#include <fe/sym.h>
#include <fe/term.h>

namespace mim {

using fe::Loc;
using fe::Pos;
using fe::Sym;

class Driver;

/// Renders Def%s with their plain Def::sym instead of Def::unique_name while alive.
/// A gid is noise in a diagnostic about the user's source - but it is also the only thing that tells two
/// same-named Def%s apart, so PlainNames::clashed reports when a message has to be rendered again with gids.
/// The state lives in Driver::names, so two Driver%s formatting at once never share it.
class PlainNames {
public:
    /// Activates plain naming on @p driver until this guard dies; a null @p driver leaves it off.
    explicit PlainNames(const Driver* driver);
    ~PlainNames();

    bool clashed() const;

    /// Registers that @p gid renders as @p sym and reports whether the plain @p sym may be used.
    /// Sets the clash flag - but still answers `true` - if another gid already claimed @p sym.
    static bool claim(const Driver&, Sym sym, uint32_t gid);

private:
    const Driver* driver_;
};

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
    };

    /// @name Constructors
    ///@{
    /// Shares @p driver's SrcMap and snapshots Flags::no_snippet from it.
    /// @note Both are what let an Error outlive @p driver: rendering must still work while an
    /// exception unwinds past the Driver's scope, whereas Error::driver_ is only ever touched while
    /// messages are added - at which point the Def%s being formatted prove their Driver is alive.
    explicit Error(const Driver& driver);
    Error() = default;
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
    /// @note Formats via `std::vformat` because a clashing pair of same-named Def%s makes us format @p s twice.
    template<class... Args>
    Error& msg(Loc loc, Tag tag, std::format_string<Args...> s, Args&&... args) {
        auto str = std::string();
        {
            auto plain = PlainNames(driver_);
            str        = std::vformat(s.get(), std::make_format_args(args...));
            if (plain.clashed()) str.clear();
        }
        if (str.empty()) str = std::vformat(s.get(), std::make_format_args(args...));

        msgs_.emplace_back(loc, tag, std::move(str));
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

    /// Error::note whose whole point is to point *elsewhere*; dropped when @p loc adds nothing.
    /// A @p loc overlapping the primary one is already covered by its snippet and so points nowhere new.
    /// The renderer gives @p loc a header line of its own, so phrase the message to stand alone.
    template<class... Args>
    Error& note_at(Loc loc, std::format_string<Args...> s, Args&&... args) {
        if (!loc || (loc & primary_loc())) return *this;
        return note(loc, s, std::forward<Args>(args)...);
    }
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

    /// Renders each Tag::Error/Tag::Warn with its source snippet and its Tag::Note%s underneath.
    /// A Tag::Note pointing at a Loc of its own reads as a diagnostic of its own - header line plus snippet;
    /// one about the primary Loc has nowhere else to point and stays a `= note:` continuation line.
    friend std::ostream& operator<<(std::ostream&, const Error&);

private:
    /// Loc of the Tag::Error/Tag::Warn that subsequent Tag::Note%s belong to.
    Loc primary_loc() const {
        for (auto i = msgs_.rbegin(), e = msgs_.rend(); i != e; ++i)
            if (i->tag != Tag::Note) return i->loc;
        return {};
    }

    std::ostream& stream(std::ostream&, const Msg&) const;

    const Driver* driver_ = nullptr; ///< Only valid while messages are added; see the constructor.
    /// Keeps the fe::Src%s alive that Msg::loc points at - a Loc renders itself, but only as long as
    /// its fe::Src is around. Never read; owning it *is* the job.
    std::shared_ptr<const fe::SrcMap> src_;
    bool no_snippet_ = false; ///< Snapshot of Flags::no_snippet, so rendering needs no Driver.
    std::vector<Msg> msgs_;
    mutable std::string what_;
};

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
    /// @note Like Loc::operator==, this only compares Loc::src by pointer identity.
    bool operator==(const Dbg& other) const noexcept { return loc_ == other.loc_ && sym_ == other.sym_; }

    template<class H>
    friend H AbslHashValue(H h, Dbg dbg) noexcept {
        return H::combine(std::move(h), dbg.loc_.src, dbg.loc_.begin.off, dbg.loc_.end.off, dbg.sym_);
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

} // namespace mim
