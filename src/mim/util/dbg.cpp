#include "mim/util/dbg.h"

#include <fe/loc.cpp.h>
#include <fe/snippet.h>

#include "mim/driver.h"

namespace mim {

namespace {

// Width of the line-number column; handed to fe::snippet so its gutter and our note prefix cannot drift apart.
constexpr auto Gutter = 5;

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

size_t plain_depth = 0;
bool plain_clashed = false;
auto plain_sym2gid = absl::flat_hash_map<Sym, uint32_t>();

} // namespace

PlainNames::PlainNames() {
    if (plain_depth++ == 0) {
        plain_clashed = false;
        plain_sym2gid.clear();
    }
}

PlainNames::~PlainNames() { --plain_depth; }

bool PlainNames::clashed() { return plain_clashed; }

bool PlainNames::claim(Sym sym, uint32_t gid) {
    if (plain_depth == 0) return false;
    if (auto [i, ins] = plain_sym2gid.emplace(sym, gid); !ins && i->second != gid) plain_clashed = true;
    return true;
}

bool Error::no_snippet() const { return driver_ && driver_->flags().no_snippet; }

void Error::clear() { msgs_.clear(); }

/// If errors occurred, claim them and throw; if warnings occurred, claim them and report to @p os.
void Error::ack(std::ostream& os) {
    auto e = std::move(*this);
    if (e.num_errors() != 0) throw e;
    if (e.num_warnings() != 0) std::print(os, "{} warning(s) encountered\n{}", e.num_warnings(), e);
}

std::ostream& operator<<(std::ostream& os, const Error& e) {
    auto src     = fe::Src();
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
            if (!e.no_snippet()) fe::snippet(os, msg.loc, src.line(msg.loc), tag2color(msg.tag), Gutter);
        }
    }

    return os.flush();
}

} // namespace mim
