#include "mim/util/dbg.h"

#include <fe/loc.cpp.h>
#include <fe/utf8.h>

#include "mim/driver.h"

namespace mim {

namespace {

// Width of the line-number column; shared by the snippet gutter and the note prefix so they cannot drift apart.
constexpr auto Gutter = 5;

/// Streams the row @p loc starts on, then a caret run underlining the columns it spans.
/// A @p loc spanning several rows underlines the remainder of its first one.
void stream_snippet(std::ostream& os, const fe::SrcFile& file, Loc loc, fe::term::FG color) {
    auto row  = file.row(loc.begin);
    auto line = file.line(row);
    if (line.empty()) return;

    auto len   = fe::utf8::num_code_points(line);
    auto begin = size_t(file.col(loc.begin)) - 1;
    if (begin >= len) return;

    auto last = file.prev(loc.end);
    auto end  = file.row(last) != row ? len : size_t(file.col(last));
    end       = std::min(std::max(end, begin + 1), len);

    os << fe::term::FG::Gray << std::format("{:>{}} | ", row, Gutter) << fe::term::FG::Reset << line << '\n'
       << fe::term::FG::Gray << std::format("{:>{}} | ", "", Gutter) << color;

    // One blank per *code point*, since that is what a column counts;
    // a tab is echoed rather than blanked, or the carets drift by its width.
    for (size_t i = 0, c = 0; c != begin && i < line.size(); ++c) {
        auto n = fe::utf8::num_bytes(char8_t(line[i]));
        os << (line[i] == '\t' ? '\t' : ' ');
        i += n == 0 ? 1 : n;
    }
    for (size_t i = begin; i != end; ++i)
        os << '^';

    os << fe::term::FG::Reset << '\n';
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
    , src_(driver.src_ptr())
    , no_snippet_(driver.flags().no_snippet) {}

std::string Error::str(Loc loc) const { return src_ ? std::format("{}", src_->at(loc)) : std::format("{}", loc); }

// Streamed piecewise instead of via std::format: a std::formatter cannot see its destination stream,
// so embedded fe::term::FG values would resolve Mode::Auto to "no color"; see fe/term.h.
std::ostream& Error::stream(std::ostream& os, const Msg& msg) const {
    os << fe::term::FG::Yellow << str(msg.loc) << ": " << msg.tag << ": " << fe::term::FG::Reset;
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

    for (const auto& msg : e.msgs()) {
        if (msg.tag == Error::Tag::Note) {
            os << fe::term::FG::Gray << std::format("{:>{}} = ", "", Gutter) << msg.tag << ": " << fe::term::FG::Reset;
            stream_code(os, msg.str);
            // The primary Loc is already spelled out above; only a note that points elsewhere repeats one.
            if (msg.loc && msg.loc != primary)
                os << fe::term::FG::Yellow << " at " << e.str(msg.loc) << fe::term::FG::Reset;
            os << '\n';
        } else {
            primary = msg.loc;
            e.stream(os, msg) << '\n';
            if (!e.no_snippet_)
                if (auto file = e.file(msg.loc)) stream_snippet(os, *file, msg.loc, tag2color(msg.tag));
        }
    }

    return os.flush();
}

} // namespace mim
