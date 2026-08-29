#include "mim/util/dbg.h"

#include <fe/snippet.h>

#include "mim/driver.h"

namespace mim {

namespace {

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
        os << fe::Snippet{loc, tag2color(tag), e.flags_.gutter, e.flags_.max_rows};
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
