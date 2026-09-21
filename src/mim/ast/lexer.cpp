#include "mim/ast/lexer.h"

#include <charconv>

#include <limits>

#include "mim/def.h" // Idx::bitwidth2size is all the IR a lexer needs

namespace mim::ast {

namespace utf8 = fe::utf8;
using Tag      = Tok::Tag;

namespace {

/// As World::lit_idx_mod but at token level: a @p mod of `0` means 2^64 and wraps nothing.
Tok idx_tok(Loc loc, u64 mod, u64 val) { return {loc, mod, mod == 0 ? val : val % mod}; }

bool is_id_head(char32_t c) { return c == '_' || utf8::isalpha(c); }
bool is_id_tail(char32_t c) { return c == '_' || utf8::isalnum(c); }

/// std::from_chars leaves @p res alone on overflow, where the widest literal is what Mim means.
u64 to_u64(std::string_view sv, int base) {
    u64 res = 0;
    auto ec = std::from_chars(sv.data(), sv.data() + sv.size(), res, base).ec;
    return ec == std::errc::result_out_of_range ? std::numeric_limits<u64>::max() : res;
}

f64 to_f64(std::string_view sv, int base) {
    f64 res = 0.;
    std::from_chars(sv.data(), sv.data() + sv.size(), res,
                    base == 16 ? std::chars_format::hex : std::chars_format::general);
    return res;
}

} // namespace

Lexer::Lexer(Driver& driver, std::string_view buf, const fe::Src* src, std::ostream* md)
    : Super(buf, src)
    , driver_(driver)
    , md_(md) {
    md_pos_ = pos();

    if (start_md())
        emit_md(true);
    else
        md_open();
}

Tok Lexer::lex() {
    while (true) {
        start();

        if (accept(utf8::EoF)) {
            if (fenced_) {
                md_flush();
                *md_ << '\n';
                md_close();
            }
            return tok(Tag::EoF);
        }
        if (accept(utf8::isspace)) continue;
        if (recover_utf8()) continue;

        // clang-format off
        // delimiters
        if (accept( '(')) return tok(Tag::D_paren_l);
        if (accept( ')')) return tok(Tag::D_paren_r);
        if (accept( '[')) return tok(Tag::D_brckt_l);
        if (accept( ']')) return tok(Tag::D_brckt_r);
        if (accept( '{')) return tok(Tag::D_brace_l);
        if (accept( '}')) return tok(Tag::D_brace_r);
        if (accept(U'«')) return tok(Tag::D_quote_l);
        if (accept(U'»')) return tok(Tag::D_quote_r);
        if (accept(U'‹')) return tok(Tag::D_angle_l);
        if (accept(U'›')) return tok(Tag::D_angle_r);
        // further tokens
        if (accept( '+')) return tok(Tag::T_add);
        if (accept( '-')) {
            if (accept('>')) return tok(Tag::T_arrow_r);
            return tok(Tag::T_sub);
        }
        if (accept(U'→')) return tok(Tag::T_arrow_r);
        if (accept(U'←')) return tok(Tag::T_arrow_l);
        if (accept( '@')) return tok(Tag::T_at);
        if (accept( '=')) {
            if (accept('>')) return tok(Tag::T_fat_arrow);
            if (accept('=')) return tok(Tag::T_eq);
            return tok(Tag::T_assign);
        }
        if (accept( '!')) {
            if (accept('=')) return tok(Tag::T_ne);
            error().e(loc_, "expected `=` after `!`");
            continue;
        }
        if (accept( '<')) {
            if (accept('<')) return tok(Tag::T_shl);
            if (accept('=')) return tok(Tag::T_le);
            if (accept('-')) return tok(Tag::T_arrow_l);
            return tok(Tag::T_lt);
        }
        if (accept( '>')) {
            if (accept('>')) return tok(Tag::T_shr);
            if (accept('=')) return tok(Tag::T_ge);
            return tok(Tag::T_gt);
        }
        if (accept(U'⊥')) return tok(Tag::T_bot);
        if (accept(U'⊤')) return tok(Tag::T_top);
        if (accept(U'□')) return tok(Tag::T_box);
        if (accept( ',')) return tok(Tag::T_comma);
        if (accept( '$')) return tok(Tag::T_dollar);
        if (accept( '#')) return tok(Tag::T_extract);
        if (accept(U'λ')) return tok(Tag::T_lm);
        if (accept( '%')) return tok(Tag::T_rem);
        if (accept( '|')) return tok(Tag::T_pipe);
        if (accept( ';')) return tok(Tag::T_semicolon);
        if (accept(U'★')) return tok(Tag::T_star);
        if (accept( '*')) return tok(Tag::T_star);
        if (accept( ':')) return tok(Tag::T_colon);
        if (accept(U'∪')) return tok(Tag::T_union);
        // clang-format on

        if (accept('.')) {
            if (accept(utf8::isdigit)) {
                lex_digits();
                lex_exp();
                return {loc_, to_f64(view(), 10)};
            }

            return tok(Tag::T_dot);
        }

        if (accept('`')) {
            if (accept(utf8::any('+', '-', '*', '/', '%'))) return {loc_, Tag::M_id, sym()};
            if (accept('<')) {
                accept(utf8::any('<', '='));
                return {loc_, Tag::M_id, sym()};
            }
            if (accept('>')) {
                accept(utf8::any('>', '='));
                return {loc_, Tag::M_id, sym()};
            }
            if (accept(utf8::any('=', '!')) && accept('=')) return {loc_, Tag::M_id, sym()};
            error().e(loc_, "expected one of `+`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`, `<<`, `>>` "
                            "after the escape hatch");
            continue;
        }

        if (accept('\'')) {
            auto c = lex_char();
            if (accept('\'')) return {loc_, c};
            error().e(loc_, "invalid character literal `{}`", view());
            continue;
        }

        if (accept('\"')) return lex_str();

        if (lex_id()) {
            auto s = sym();
            if (auto tag = driver().keys().find(s)) return {loc_, *tag};
            return {loc_, Tag::M_id, s};
        }

        if (utf8::isdigit(ahead())) return lex_lit();

        if (start_md()) {
            emit_md();
            continue;
        }

        // comments
        if (accept('/')) {
            if (accept('*')) {
                eat_comments();
                continue;
            }
            if (accept('/')) {
                accept_while([](char32_t c) { return c != '\n'; });
                continue;
            }

            return tok(Tag::T_div);
        }

        recover_char();
    }
}

bool Lexer::lex_id() {
    if (accept(is_id_head)) {
        accept_while(is_id_tail);
        return true;
    }
    return false;
}

std::string Lexer::escape(std::string_view str) {
    std::string res;
    for (auto c : str) {
        // clang-format off
        switch (c) {
            case '\\': res += "\\\\"; break;
            case '\"': res += "\\\""; break;
            case '\0': res += "\\0";  break;
            case '\a': res += "\\a";  break;
            case '\b': res += "\\b";  break;
            case '\f': res += "\\f";  break;
            case '\n': res += "\\n";  break;
            case '\r': res += "\\r";  break;
            case '\t': res += "\\t";  break;
            case '\v': res += "\\v";  break;
            default:   res += c;
        }
        // clang-format on
    }
    return res;
}

bool Lexer::is_id(std::string_view str) {
    size_t i = 0;
    if (!is_id_head(utf8::decode(str, i))) return false;
    while (i != str.size())
        if (!is_id_tail(utf8::decode(str, i))) return false;
    return true;
}

// clang-format off
Tok Lexer::lex_lit() {
    int base = 10;

    // prefix starting with '0'
    if (accept('0')) {
        if      (accept(utf8::any('b', 'B'))) base =  2;
        else if (accept(utf8::any('o', 'O'))) base =  8;
        else if (accept(utf8::any('x', 'X'))) base = 16;
    }

    // Everything the prefix does not cover; std::from_chars wants a hexadecimal float without its `0x`.
    auto begin = loc_.end.off;
    auto body  = [&](uint32_t end) { return buf_.substr(begin, end - begin); };

    lex_digits(base);
    auto end = loc_.end.off;

    if (accept(utf8::any('i', 'I'))) {
        auto val   = to_u64(body(end), base);
        auto i     = loc_.end.off;
        lex_digits();
        auto width = to_u64(buf_.substr(i, loc_.end.off - i), 10);
        return Tok{loc_, Idx::bitwidth2size(width), val};
    }

    if (base == 10) {
        if (utf8::isrange(ahead(), U'₀', U'₉')) {
            auto i = to_u64(body(end), 10);
            std::string mod;
            while (utf8::isrange(ahead(), U'₀', U'₉')) mod += char(next() - U'₀' + '0');
            return idx_tok(loc_, to_u64(mod, 10), i);
        } else if (accept('_')) {
            auto i = to_u64(body(end), 10);
            auto m = loc_.end.off;
            if (accept(utf8::isdigit)) {
                lex_digits(10);
                return idx_tok(loc_, to_u64(buf_.substr(m, loc_.end.off - m), 10), i);
            } else {
                error().e(loc_, "stray underscore in Idx literal; size is missing");
                return Tok{loc_, i};
            }
        }
    }

    bool is_float = false;
    if (base == 10 || base == 16) {
        // parse fractional part
        if (accept('.')) {
            is_float = true;
            lex_digits(base);
        }

        bool has_exp = lex_exp(base);
        if (base == 16 && is_float && !has_exp) error().e(loc_, "hexadecimal floating constants require an exponent");
        is_float |= has_exp;
    }

    if (is_float) return Tok{loc_, to_f64(body(loc_.end.off), base)};
    else          return Tok{loc_, to_u64(body(end),          base)};
}

void Lexer::lex_digits(int base /*= 10*/) {
    switch (base) {
        // clang-format off
        case  2: accept_while(utf8::isbdigit); break;
        case  8: accept_while(utf8::isodigit); break;
        case 10: accept_while(utf8::isdigit);  break;
        case 16: accept_while(utf8::isxdigit); break;
        // clang-format on
        default: fe::unreachable();
    }
}

bool Lexer::lex_exp(int base /*= 10*/) {
    if (accept(base == 10 ? utf8::any('e', 'E') : utf8::any('p', 'P'))) {
        accept(utf8::any('+', '-'));
        if (!utf8::isdigit(ahead())) error().e(loc_, "exponent has no digits");
        lex_digits();
        return true;
    }
    return false;
}
// clang-format on

char8_t Lexer::lex_char() {
    if (accept('\\')) {
        // clang-format off
        switch (auto c = ahead()) {
            case '\'': next(); return '\'';
            case '\\': next(); return '\\';
            case  '"': next(); return '\"';
            case  '0': next(); return '\0';
            case  'a': next(); return '\a';
            case  'b': next(); return '\b';
            case  'f': next(); return '\f';
            case  'n': next(); return '\n';
            case  'r': next(); return '\r';
            case  't': next(); return '\t';
            case  'v': next(); return '\v';
            default:
                if (c != utf8::EoF) error().e(loc_.anew_end(), "invalid escape character `\\{}`", (char)c);
                return '\0';
        }
        // clang-format on
    }

    auto c = next();
    if (utf8::isascii(c)) return char8_t(c);
    error().e(loc_, "invalid character `{}`", utf8::Char32(c));
    return '\0';
}

/// The body is a slice of Lexer::buf_ unless an escape made it diverge - see Lexer::unquote.
Tok Lexer::lex_str() {
    auto begin = loc_.end.off; // just past the opening `"`
    bool esc   = false;

    while (true) {
        if (accept('\"')) return {loc_, Tag::L_str, sym_str(begin, loc_.end.off - 1, esc)};

        if (ahead() == utf8::EoF) {
            error().e(loc_, "unterminated string literal");
            return {loc_, Tag::L_str, sym_str(begin, loc_.end.off, esc)};
        }

        if (accept('\\')) {
            esc = true;
            if (ahead() != utf8::EoF) next();
        } else if (auto c = next(); !utf8::isascii(c)) {
            error().e(loc_, "invalid character `{}`", utf8::Char32(c));
        }
    }
}

Sym Lexer::sym_str(uint32_t begin, uint32_t end, bool esc) {
    auto body = buf_.substr(begin, end - begin);
    return driver().sym(esc ? std::string_view(unquote(body, begin)) : body);
}

std::string Lexer::unquote(std::string_view body, uint32_t begin) {
    std::string res;
    res.reserve(body.size());

    for (size_t i = 0, e = body.size(); i != e; ++i) {
        auto c = body[i];
        if (c == '\\' && i + 1 != e) {
            // clang-format off
            switch (body[++i]) {
                case '\'': res += '\''; break;
                case '\\': res += '\\'; break;
                case  '"': res += '\"'; break;
                case  '0': res += '\0'; break;
                case  'a': res += '\a'; break;
                case  'b': res += '\b'; break;
                case  'f': res += '\f'; break;
                case  'n': res += '\n'; break;
                case  'r': res += '\r'; break;
                case  't': res += '\t'; break;
                case  'v': res += '\v'; break;
                // clang-format on
                default:
                    auto loc = Loc(src_, Pos(begin + u32(i) - 1), Pos(begin + u32(i) + 1));
                    error().e(loc, "invalid escape character `\\{}`", body[i]);
            }
        } else {
            res += c;
        }
    }

    return res;
}

void Lexer::eat_comments() {
    while (true) {
        accept_while([](char32_t c) { return c != '*'; });
        if (accept(utf8::EoF)) {
            error().e(loc_, "unterminated multi-line comment");
            return;
        }
        next();
        if (accept('/')) break;
    }
}

void Lexer::emit_md(bool start_of_file) {
    if (!start_of_file) {
        md_flush();
        md_close();
    }

    do {
        for (int i = 0; i != 3; ++i)
            next();
        accept(' ');
        md_skip();

        accept_while([](char32_t c) { return c != '\n'; });
        accept('\n');
        md_flush();
    } while (start_md());

    if (ahead() != utf8::EoF) md_open();
}

} // namespace mim::ast
