#include "mim/util/dbg.h"

#include <fstream>

#include <fe/loc.cpp.h>
#include <fe/utf8.h>

#include "mim/driver.h"

namespace mim {

namespace {

/// The source lines an Error refers to, read on demand and memoized per file.
/// Scoped to a single rendering: Loc::path is owned by the Driver, so nothing may outlive it.
class Src {
public:
    /// The line Loc::begin points at, or empty if the file or that row is unavailable.
    std::string_view line(Loc loc) {
        if (!loc || !loc.path) return {};

        auto [i, ins] = path2lines_.try_emplace(loc.path);
        if (ins) {
            auto ifs = std::ifstream(*loc.path);
            for (std::string line; std::getline(ifs, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                i->second.emplace_back(std::move(line));
            }
        }

        auto row = loc.begin.row;
        return row <= i->second.size() ? std::string_view(i->second[row - 1]) : std::string_view();
    }

private:
    absl::flat_hash_map<const fs::path*, std::vector<std::string>> path2lines_;
};

// Width of the line-number column; shared by the snippet gutter and the note prefix so they cannot drift apart.
constexpr auto Gutter = 5;

/// Streams @p line, then a caret run underlining the columns @p loc spans, both behind a Gutter-wide row number.
/// A @p loc spanning several rows underlines the remainder of its first one.
void stream_snippet(std::ostream& os, Loc loc, std::string_view line, fe::term::FG color) {
    // A col of 0 is Pos's "unknown" sentinel, so there is nothing to point at.
    if (line.empty() || loc.begin.col == 0) return;

    auto len   = fe::utf8::num_code_points(line);
    auto begin = size_t(loc.begin.col) - 1;
    if (begin >= len) return;

    auto finis = loc.finis.row != loc.begin.row || loc.finis.col == 0 ? len : size_t(loc.finis.col);
    finis      = std::min(std::max(finis, begin + 1), len);

    os << fe::term::FG::Gray << std::format("{:>{}} | ", loc.begin.row, Gutter) << fe::term::FG::Reset << line << '\n'
       << fe::term::FG::Gray << std::format("{:>{}} | ", "", Gutter) << color;

    // One blank per *code point*, since that is what Pos::col counts;
    // a tab is echoed rather than blanked, or the carets drift by its width.
    for (size_t i = 0, c = 0; c != begin && i < line.size(); ++c) {
        auto n = fe::utf8::num_bytes(char8_t(line[i]));
        os << (line[i] == '\t' ? '\t' : ' ');
        i += n == 0 ? 1 : n;
    }
    for (size_t i = begin; i != finis; ++i)
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
    , no_snippet_(driver.flags().no_snippet) {}

void Error::clear() { msgs_.clear(); }

/// If errors occurred, claim them and throw; if warnings occurred, claim them and report to @p os.
void Error::ack(std::ostream& os) {
    auto e = std::move(*this);
    if (e.num_errors() != 0) throw e;
    if (e.num_warnings() != 0) std::print(os, "{} warning(s) encountered\n{}", e.num_warnings(), e);
}

std::ostream& operator<<(std::ostream& os, const Error& e) {
    auto src     = Src();
    auto primary = Loc();

    for (const auto& msg : e.msgs()) {
        if (msg.tag == Error::Tag::Note) {
            os << fe::term::FG::Gray << std::format("{:>{}} = ", "", Gutter) << msg.tag << ": " << fe::term::FG::Reset;
            stream_code(os, msg.str);
            // The primary Loc is already spelled out above; only a note that points elsewhere repeats one.
            if (msg.loc && msg.loc != primary) os << fe::term::FG::Yellow << " at " << msg.loc << fe::term::FG::Reset;
            os << '\n';
        } else {
            primary = msg.loc;
            os << msg << '\n';
            if (!e.no_snippet_) stream_snippet(os, msg.loc, src.line(msg.loc), tag2color(msg.tag));
        }
    }

    return os.flush();
}

} // namespace mim
