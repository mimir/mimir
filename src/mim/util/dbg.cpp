#include "mim/util/dbg.h"

#include <fe/loc.cpp.h>
#include <fe/utf8.h>

#include "mim/driver.h"

namespace mim {

namespace {

/// Streams @p row of the source and a caret run underlining the code points in `[begin, end)`.
void stream_row(std::ostream& os,
                std::string_view line,
                uint32_t row,
                size_t begin,
                size_t end,
                fe::term::FG color,
                uint32_t gutter) {
    os << fe::term::FG::Gray << std::format("{:>{}} | ", row, gutter) << fe::term::FG::Reset << line << '\n';
    if (end <= begin) return;

    os << fe::term::FG::Gray << std::format("{:>{}} | ", "", gutter) << color;

    // One blank per *code point*, since that is what a column counts;
    // a tab is echoed rather than blanked, or the carets drift by its width.
    for (size_t i = 0, c = 0; c != begin && i < line.size(); ++c) {
        bool tab = line[i] == '\t';
        fe::utf8::decode(line, i);
        os << (tab ? '\t' : ' ');
    }
    for (size_t i = begin; i != end; ++i)
        os << '^';

    os << fe::term::FG::Reset << '\n';
}

/// Streams every row @p loc spans, each underlining the columns @p loc covers on it.
void stream_snippet(std::ostream& os, const fe::Src& src, Loc loc, fe::term::FG color, const Flags& flags) {
    auto [first_row, first_col] = src.rowcol(loc.begin);
    auto [last_row, last_col]   = src.rowcol(src.prev(loc.end));
    if (first_row == 0) return;
    if (last_row < first_row) last_row = first_row, last_col = first_col;

    if (first_row == last_row && size_t(first_col) - 1 >= fe::utf8::num_code_points(src.line(first_row))) return;

    for (auto row = first_row; row <= last_row; ++row) {
        if (auto max = flags.max_rows; max != 0 && last_row - first_row + 1 > max && row == first_row + max / 2) {
            os << fe::term::FG::Gray << std::format("{:>{}} |\n", "...", flags.gutter) << fe::term::FG::Reset;
            row = last_row - (max - max / 2 - 1);
        }

        auto line  = src.line(row);
        auto len   = fe::utf8::num_code_points(line);
        auto begin = std::min(row == first_row ? size_t(first_col) - 1 : 0, len);
        auto end   = std::min(std::max(row == last_row ? size_t(last_col) : len, begin + 1), len);
        stream_row(os, line, row, begin, end, color, flags.gutter);
    }
}

fe::term::FG tag2color(Error::Tag tag) {
    // clang-format off
    switch (tag) {
        case Error::Tag::Error: return fe::term::FG::Red;
        case Error::Tag::Warn:  return fe::term::FG::Magenta;
        case Error::Tag::Note:  return fe::term::FG::Green;
        default: fe::unreachable();
    }
    // clang-format on
}

} // namespace

PlainNames::PlainNames(const Driver* driver)
    : driver_(driver) {
    if (!driver_) return;

    auto& names = driver_->names();
    if (names.depth++ == 0) {
        names.clashed = false;
        names.sym2gid.clear();
    }
}

PlainNames::~PlainNames() {
    if (driver_) --driver_->names().depth;
}

bool PlainNames::clashed() const { return driver_ && driver_->names().clashed; }

bool PlainNames::claim(const Driver& driver, Sym sym, uint32_t gid) {
    auto& names = driver.names();
    if (names.depth == 0) return false;
    if (auto [i, ins] = names.sym2gid.emplace(sym, gid); !ins && i->second != gid) names.clashed = true;
    return true;
}

Error::Error(const Driver& driver)
    : driver_(&driver)
    , flags_(driver.flags()) {}

// Streamed piecewise instead of via std::format: a std::formatter cannot see its destination stream,
// so embedded fe::term::FG values would resolve Mode::Auto to "no color"; see fe/term.h.
std::ostream& Error::stream(std::ostream& os, const Msg& msg) const {
    os << fe::term::FG::Yellow << msg.loc << ": " << msg.tag << ": " << fe::term::FG::Reset;
    return stream_code(os, msg.str);
}

void Error::clear() { msgs_.clear(); }

/// If errors occurred, claim them and throw; if warnings occurred, claim them and report to @p os.
void Error::ack(std::ostream& os) {
    auto e = std::move(*this);
    if (e.num_errors() != 0) throw e;
    if (e.num_warnings() != 0) std::print(os, "{} warning(s) encountered\n{}", e.num_warnings(), e);
}

std::ostream& operator<<(std::ostream& os, const Error& e) {
    auto primary = Loc();

    auto snippet = [&](Loc loc, Error::Tag tag) {
        if (e.flags_.no_snippet) return;
        if (auto src = loc.src) stream_snippet(os, *src, loc, tag2color(tag), e.flags_);
    };

    for (const auto& msg : e.msgs()) {
        if (msg.tag == Error::Tag::Note) {
            // A note pointing elsewhere reads as a diagnostic of its own; one about the primary Loc has no
            // other place to name and stays a continuation line.
            if (msg.loc && msg.loc != primary) {
                os << std::format("{:>{}} ", "", e.flags_.gutter);
                e.stream(os, msg) << '\n';
                snippet(msg.loc, msg.tag);
            } else {
                os << fe::term::FG::Gray << std::format("{:>{}} = ", "", e.flags_.gutter) << msg.tag << ": "
                   << fe::term::FG::Reset;
                stream_code(os, msg.str) << '\n';
            }
        } else {
            primary = msg.loc;
            e.stream(os, msg) << '\n';
            snippet(msg.loc, msg.tag);
        }
    }

    return os.flush();
}

} // namespace mim
