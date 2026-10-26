#include "mim/manifest.h"

#include <cstdlib>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fe/assert.h>
#include <fe/driver.h>
#include <fe/format.h>
#include <fe/lexer.h>
#include <fe/parser.h>
#include <fe/sys.h>
#include <fe/term.h>
#include <fe/utf8.h>

namespace fs = std::filesystem;

namespace mim::manifest {

namespace utf8 = fe::utf8;
using fe::Loc;
using fe::Pos;

/// Token type of the `mim.toml` lexer; satisfies fe::Token (see fe::Parser).
class Tok {
public:
    enum class Tag {
        Nil, ///< The "failure" token of a failed Parser::expect.
        EoF,
        M_id,
        L_str, ///< Tok::str yields its unescaped value
        D_brckt_l,
        D_brckt_r,
        T_assign,
        T_comma,
    };

    static constexpr std::string_view tag2str(Tag tag) {
        switch (tag) {
            case Tag::Nil: return "<nil>";
            case Tag::EoF: return "<end of file>";
            case Tag::M_id: return "<key>";
            case Tag::L_str: return "<string>";
            case Tag::D_brckt_l: return "[";
            case Tag::D_brckt_r: return "]";
            case Tag::T_assign: return "=";
            case Tag::T_comma: return ",";
        }
        std::unreachable();
    }

    Tok() {}
    Tok(Loc loc, Tag tag)
        : loc_(loc)
        , tag_(tag) {}
    Tok(Loc loc, Tag tag, std::string str)
        : loc_(loc)
        , tag_(tag)
        , str_(std::move(str)) {
        assert(tag == Tag::M_id || tag == Tag::L_str);
    }

    Tag tag() const { return tag_; }
    bool isa(Tag tag) const { return tag_ == tag; }
    Loc loc() const { return loc_; }
    const std::string& str() const {
        assert(tag_ == Tag::M_id || tag_ == Tag::L_str);
        return str_;
    }
    explicit operator bool() const { return tag_ != Tag::Nil; }

    friend std::ostream& operator<<(std::ostream& os, Tag tag) { return os << tag2str(tag); }
    friend std::ostream& operator<<(std::ostream& os, Tok tok) {
        switch (tok.tag_) {
            case Tag::M_id: return os << tok.str_;
            case Tag::L_str: return os << '"' << tok.str_ << '"';
            default: return os << tag2str(tok.tag_);
        }
    }

private:
    Loc loc_;
    Tag tag_ = Tag::Nil;
    std::string str_;
};

} // namespace mim::manifest

#ifndef DOXYGEN // clang-format off
template<> struct std::formatter<mim::manifest::Tok     > : fe::ostream_formatter {};
template<> struct std::formatter<mim::manifest::Tok::Tag> : fe::ostream_formatter {};
#endif // clang-format on

namespace mim::manifest {

class Lexer : public fe::Lexer<1, Lexer> {
public:
    Lexer(fe::Driver& driver, const fe::Src& src)
        : fe::Lexer<1, Lexer>(src)
        , driver_(driver) {}

    fe::Driver& driver() { return driver_; }
    Tok lex();

private:
    Tok lex_str(char32_t quote);

    fe::Driver& driver_;
};

class Parser : public fe::Parser<Tok, Tok::Tag, 1, Parser> {
public:
    Parser(fe::Driver& driver, const fe::Src& src)
        : lexer_(driver, src) {
        init();
    }

    Lexer& lexer() { return lexer_; }
    fe::Driver& driver() { return lexer_.driver(); }

    Manifest parse();

private:
    Lexer lexer_;
};

namespace {

std::optional<char> unescape(char32_t c) {
    switch (c) {
        case '"': return '"';
        case '\\': return '\\';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        default: return {}; // TODO: maybe add more escapes
    }
}

bool is_key_char(char32_t c) { return utf8::isalnum(c) || c == '_' || c == '-'; }

} // namespace

Tok Lexer::lex() {
    while (true) {
        start();

        if (accept(utf8::EoF)) return {loc_, Tok::Tag::EoF};
        if (accept(utf8::isspace)) continue;
        if (recover_utf8()) continue;

        if (accept('[')) return {loc_, Tok::Tag::D_brckt_l};
        if (accept(']')) return {loc_, Tok::Tag::D_brckt_r};
        if (accept(',')) return {loc_, Tok::Tag::T_comma};
        if (accept('=')) return {loc_, Tok::Tag::T_assign};

        if (accept('#')) {
            accept_while_none_of(char8_t('\r'), char8_t('\n'));
            continue;
        }

        if (accept('"')) return lex_str('"');
        if (accept('\'')) return lex_str('\'');

        if (accept(is_key_char)) {
            accept_while(is_key_char);
            return {loc_, Tok::Tag::M_id, std::string(view())};
        }

        recover_char();
    }
}

Tok Lexer::lex_str(char32_t quote) {
    std::string res;

    while (true) {
        if (accept(quote)) return {loc_, Tok::Tag::L_str, std::move(res)};

        if (eol()) {
            error().e(loc_, "unterminated string literal");
            return {loc_, Tok::Tag::L_str, std::move(res)};
        }

        if (quote == '"' && accept('\\')) {
            if (eol()) continue;
            auto backslash = loc_.end.off - 1;
            if (auto u = unescape(next())) {
                res += *u;
            } else {
                auto escaped = buf_.substr(backslash + 1, loc_.end.off - backslash - 1);
                error().e(Loc(src_, Pos(backslash), loc_.end), "invalid escape character `\\{}`", escaped);
                res += escaped;
            }
        } else {
            auto begin = loc_.end.off;
            next();
            res += buf_.substr(begin, loc_.end.off - begin);
        }
    }
}

struct List_Entry {
    std::string_view key;
    fe::Vector<std::string> Manifest::* list;
};

constexpr auto lists = {
    List_Entry{     Plugin_Key,      &Manifest::plugins},
    List_Entry{Plugin_Path_Key, &Manifest::plugin_paths},
    List_Entry{Import_Path_Key, &Manifest::import_paths},
    List_Entry{Prefix_Path_Key, &Manifest::prefix_paths},
    List_Entry{ Plugin_Arg_Key,  &Manifest::plugin_args}
};

Manifest Parser::parse() {
    Manifest manifest{};
    std::array<bool, lists.size()> seen{};

    while (!ahead().isa(Tok::Tag::EoF)) {
        auto key = expect(Tok::Tag::M_id, "manifest entry");
        if (!key) {
            lex();
            continue;
        }
        auto entry = std::ranges::find(lists, key.str(), &List_Entry::key);
        if (entry == lists.end()) {
            auto keys = std::ranges::fold_left(
                std::views::drop(lists, 1), fe::format_cite("`{}`", lists.begin()->key),
                [](fe::Cited acc, const auto& e) { return fe::format_cite("{}, `{}`", acc, e.key); });
            error().e(key.loc(), "unknown manifest key `{}`", key.str()).n("expected one of {}", keys);
        } else {
            auto i = entry - lists.begin();
            if (seen[i]) error().e(key.loc(), "duplicate manifest key `{}`", key.str());
            seen[i] = true;
        }

        expect(Tok::Tag::T_assign, "manifest entry");

        auto brckt = expect(Tok::Tag::D_brckt_l, "value of key `{}`", key);
        if (!brckt) {
            lex();
            continue;
        }
        auto _ = anchor(brckt, Tok::Tag::D_brckt_r);

        for (bool first = true; !ahead().isa(Tok::Tag::EoF) && !ahead().isa(Tok::Tag::D_brckt_r);) {
            if (!first) {
                expect(Tok::Tag::T_comma, "value of key `{}`", key);
                if (ahead().isa(Tok::Tag::D_brckt_r)) break;
            }
            first = false;

            auto val = expect(Tok::Tag::L_str, "value of key `{}`", key);
            if (!val) {
                lex();
                continue;
            }
            if (entry == lists.end()) continue;

            if (key.str() == Plugin_Arg_Key && !val.str().contains(':'))
                error().e(val.loc(), "expected `<plugin>:<arg>`, got `{}`", val.str());

            auto& dst = std::invoke(entry->list, &manifest);
            dst.emplace_back(std::move(val.str()));
        }

        expect(Tok::Tag::D_brckt_r, "value of key `{}`", key);
    }

    return manifest;
}

} // namespace mim::manifest

namespace mim {

Manifest load_manifest(const fs::path& file) {
    std::error_code ec{};
    if (!fs::is_regular_file(file, ec))
        fe::throwf<std::invalid_argument>("cannot read config file `{}`", file.string());

    fe::Driver driver;
    auto src = driver.src().add(file).first;
    if (!src) fe::throwf<std::invalid_argument>("cannot read config file `{}`", file.string());

    manifest::Parser parser{driver, *src};
    Manifest manifest = parser.parse();
    try {
        driver.error().ack();
    } catch (const fe::Error::Bail& bail) { throw std::invalid_argument(bail.what()); }

    // Relative `*-path` entries anchor to the manifest file's directory.
    // `paths` and `p` are references, so assigning to `p` rewrites the manifest entry in place.
    auto resolve = [base = file.parent_path()](fe::Vector<std::string>& paths) {
        for (auto& p : paths)
            if (auto fp = fs::path(p); !fp.is_absolute()) p = (base / fp).lexically_normal().string();
    };
    resolve(manifest.plugin_paths);
    resolve(manifest.import_paths);
    resolve(manifest.prefix_paths);
    return manifest;
}

fe::Vector<std::string> paths_from_env(const char* var) {
    if (auto env = std::getenv(var))
        return std::views::split(std::string_view(env), fe::sys::Path_Sep)
             | std::views::transform([](auto&& r) { return std::string(r.begin(), r.end()); })
             | std::views::filter([](auto&& p) { return !p.empty(); }) | std::ranges::to<fe::Vector<std::string>>();
    return {};
}

} // namespace mim
