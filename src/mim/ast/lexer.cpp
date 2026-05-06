#include "mim/ast/lexer.h"

#include "mim/ast/ast.h"

using namespace std::literals;

namespace mim::ast {

namespace utf8 = fe::utf8;
using Tag      = Tok::Tag;

Lexer::Lexer(AST& ast, std::istream& istream, const fs::path* path /*= nullptr*/, std::ostream* md /*= nullptr*/)
    : Super(istream, path)
    , ast_(ast)
    , md_(md) {
#define CODE(t, str) keywords_[ast.sym(str)] = Tag::t;
    MIM_KEY(CODE)
#undef CODE

#define CODE(str, t) \
    if (Tag::t != Tag::Nil) keywords_[ast.sym(str)] = Tag::t;
    MIM_SUBST(CODE)
#undef CODE

    if (start_md())
        emit_md(true);
    else
        md_fence();
}

Tok Lexer::lex() {
    while (true) {
        start();

        if (accept(utf8::EoF)) return tok(Tag::EoF);
        if (accept(utf8::isspace)) continue;
        if (accept(utf8::Invalid)) {
            ast().error(loc_, "invalid UTF-8 character");
            continue;
        }

        // clang-format off
        // delimiters
        if (accept( '(')) return tok(Tag::D_paren_l);
        if (accept( ')')) return tok(Tag::D_paren_r);
        if (accept( '[')) return tok(Tag::D_brckt_l);
        if (accept( ']')) return tok(Tag::D_brckt_r);
        if (accept( '{')) return tok(Tag::D_brace_l);
        if (accept( '}')) return tok(Tag::D_brace_r);
        if (accept(U'⦃')) return tok(Tag::D_curly_l);
        if (accept(U'⦄')) return tok(Tag::D_curly_r);
        if (accept(U'«')) return tok(Tag::D_quote_l);
        if (accept(U'»')) return tok(Tag::D_quote_r);
        if (accept(U'⟪')) return tok(Tag::D_quote_l);
        if (accept(U'⟫')) return tok(Tag::D_quote_r);
        if (accept(U'‹')) return tok(Tag::D_angle_l);
        if (accept(U'›')) return tok(Tag::D_angle_r);
        if (accept(U'⟨')) return tok(Tag::D_angle_l);
        if (accept(U'⟩')) return tok(Tag::D_angle_r);
        if (accept( '<')) {
            if (accept( '<')) return tok(Tag::D_quote_l);
            return tok(Tag::D_angle_l);
        }
        if (accept( '>')) {
            if (accept( '>')) return tok(Tag::D_quote_r);
            return tok(Tag::D_angle_r);
        }
        // further tokens
        if (accept(U'→')) return tok(Tag::T_arrow);
        if (accept( '@')) return tok(Tag::T_at);
        if (accept( '=')) {
            if (accept('>')) return tok(Tag::T_fat_arrow);
            return tok(Tag::T_assign);
        }
        if (accept(U'⊥')) return tok(Tag::T_bot);
        if (accept(U'⊤')) return tok(Tag::T_top);
        if (accept(U'□')) return tok(Tag::T_box);
        if (accept( ',')) return tok(Tag::T_comma);
        if (accept( '$')) return tok(Tag::T_dollar);
        if (accept( '#')) return tok(Tag::T_extract);
        if (accept(U'λ')) return tok(Tag::T_lm);
        if (accept( '|')) return tok(Tag::T_pipe);
        if (accept( ';')) return tok(Tag::T_semicolon);
        if (accept(U'★')) return tok(Tag::T_star);
        if (accept( '*')) return tok(Tag::T_star);
        if (accept( ':')) return tok(Tag::T_colon);
        if (accept(U'∪')) return tok(Tag::T_union);
        // clang-format on

        if (accept('%')) {
            if (lex_id()) return {loc_, Tag::M_anx, sym()};
            ast().error(loc_, "invalid axm name '{}'", str_);
            continue;
        }

        if (accept('.')) {
            if (accept(utf8::isdigit)) {
                parse_digits();
                parse_exp();
                return {loc_, f64(std::strtod(str_.c_str(), nullptr))};
            }

            return tok(Tag::T_dot);
        }

        if (accept('\'')) {
            auto c = lex_char();
            if (accept('\'')) return {loc_, c};
            ast().error(loc_, "invalid character literal {}", str_);
            continue;
        }

        if (accept<Append::Off>('\"')) {
            while (lex_char() != '"') {}
            str_.pop_back(); // remove final '"'
            return {loc_, Tag::L_str, sym()};
        }

        if (lex_id()) {
            if (auto i = keywords_.find(sym()); i != keywords_.end()) return tok(i->second);
            return {loc_, Tag::M_id, sym()};
        }

        if (utf8::isdigit(ahead()) || utf8::any('+', '-')(ahead())) {
            if (auto lit = parse_lit()) return *lit;
            continue;
        }

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
                while (ahead() != utf8::EoF && ahead() != '\n')
                    next();
                continue;
            }

            ast().error({loc_.path, peek_}, "invalid input char '/'; maybe you wanted to start a comment?");
            continue;
        }

        ast().error({loc_.path, peek_}, "invalid input char '{}'", utf8::Char32(ahead()));
        next();
    }
}

bool Lexer::lex_id() {
    if (accept([](char32_t c) { return c == '_' || utf8::isalpha(c); })) {
        while (accept([](char32_t c) { return c == '_' || c == '.' || utf8::isalnum(c); })) {}
        return true;
    }
    return false;
}

// clang-format off
std::optional<Tok> Lexer::parse_lit() {
    int base = 10;
    std::optional<bool> sign;

    if (accept<Append::Off>('+')) {
        sign = false;
    } else if (accept<Append::Off>('-')) {
        if (accept('>')) return tok(Tag::T_arrow);
        sign = true;
    }

    // prefix starting with '0'
    if (accept<Append::Off>('0')) {
        if      (accept<Append::Off>('b')) base =  2;
        else if (accept<Append::Off>('B')) base =  2;
        else if (accept<Append::Off>('o')) base =  8;
        else if (accept<Append::Off>('O')) base =  8;
        else if (accept<Append::Off>('x')) base = 16;
        else if (accept<Append::Off>('X')) base = 16;
    }

    parse_digits(base);

    if (accept(utf8::any('i', 'I'))) {
        if (sign) str_.insert(0, "-"sv);
        auto val = std::strtoull(str_.c_str(), nullptr, base);
        str_.clear();
        parse_digits();
        auto width = std::strtoull(str_.c_str(), nullptr, 10);
        return Tok{loc_, ast().world().lit_int(width, val)};
    }

    if (!sign && base == 10) {
        if (utf8::isrange(ahead(), U'₀', U'₉')) {
            auto i = std::strtoull(str_.c_str(), nullptr, 10);
            std::string mod;
            while (utf8::isrange(ahead(), U'₀', U'₉')) mod += next() - U'₀' + '0';
            auto m = std::strtoull(mod.c_str(), nullptr, 10);
            return Tok{loc_, ast().world().lit_idx_mod(m, i)};
        } else if (accept<Append::Off>('_')) {
            auto i = std::strtoull(str_.c_str(), nullptr, 10);
            str_.clear();
            if (accept(utf8::isdigit)) {
                parse_digits(10);
                auto m = std::strtoull(str_.c_str(), nullptr, 10);
                return Tok{loc_, ast().world().lit_idx_mod(m, i)};
            } else {
                ast().error(loc_, "stray underscore in Idx literal; size is missing");
                auto i = std::strtoull(str_.c_str(), nullptr, 10);
                return Tok{loc_, u64(i)};
            }
        }
    }

    bool is_float = false;
    if (base == 10 || base == 16) {
        // parse fractional part
        if (accept('.')) {
            is_float = true;
            parse_digits(base);
        }

        bool has_exp = parse_exp(base);
        if (base == 16 && is_float && !has_exp) ast().error(loc_, "hexadecimal floating constants require an exponent");
        is_float |= has_exp;
    }

    if (sign && str_.empty()) {
        ast().error(loc_, "stray '{}'", *sign ? "-" : "+");
        return {};
    }

    if (is_float && base == 16) str_.insert(0, "0x"sv);
    if (sign && *sign) str_.insert(0, "-"sv);

    if (is_float) return Tok{loc_, f64(std::strtod  (str_.c_str(), nullptr      ))};
    if (sign)     return Tok{loc_, u64(std::strtoll (str_.c_str(), nullptr, base))};
    else          return Tok{loc_, u64(std::strtoull(str_.c_str(), nullptr, base))};
}

void Lexer::parse_digits(int base /*= 10*/) {
    switch (base) {
        // clang-format off
        case  2: while (accept(utf8::isbdigit)) {} break;
        case  8: while (accept(utf8::isodigit)) {} break;
        case 10: while (accept(utf8::isdigit))  {} break;
        case 16: while (accept(utf8::isxdigit)) {} break;
        // clang-format on
        default: fe::unreachable();
    }
}

bool Lexer::parse_exp(int base /*= 10*/) {
    if (accept(base == 10 ? utf8::any('e', 'E') : utf8::any('p', 'P'))) {
        accept(utf8::any('+', '-'));
        if (!utf8::isdigit(ahead())) ast().error(loc_, "exponent has no digits");
        parse_digits();
        return true;
    }
    return false;
}
// clang-format on

char8_t Lexer::lex_char() {
    if (accept<Append::Off>('\\')) {
        // clang-format off
        if (false) {}
        else if (accept<Append::Off>('\'')) str_ += '\'';
        else if (accept<Append::Off>('\\')) str_ += '\\';
        else if (accept<Append::Off>( '"')) str_ += '\"';
        else if (accept<Append::Off>( '0')) str_ += '\0';
        else if (accept<Append::Off>( 'a')) str_ += '\a';
        else if (accept<Append::Off>( 'b')) str_ += '\b';
        else if (accept<Append::Off>( 'f')) str_ += '\f';
        else if (accept<Append::Off>( 'n')) str_ += '\n';
        else if (accept<Append::Off>( 'r')) str_ += '\r';
        else if (accept<Append::Off>( 't')) str_ += '\t';
        else if (accept<Append::Off>( 'v')) str_ += '\v';
        else ast().error(loc_.anew_finis(), "invalid escape character '\\{}'", (char)ahead());
        // clang-format on
        return str_.back();
    }
    auto c = next();
    str_ += c;
    if (utf8::isascii(c)) return char8_t(c);
    ast().error(loc_, "invalid character '{}'", (char)c);
    return '\0';
}

void Lexer::eat_comments() {
    while (true) {
        while (ahead() != utf8::EoF && ahead() != '*')
            next();
        if (accept(utf8::EoF)) {
            ast().error(loc_, "non-terminated multiline comment");
            return;
        }
        next();
        if (accept('/')) break;
    }
}

void Lexer::emit_md(bool start_of_file) {
    if (!start_of_file) md_fence();

    do {
        out_ = false;
        for (int i = 0; i < 3; ++i)
            next();
        accept(' ');
        out_ = true;

        while (ahead() != utf8::EoF && ahead() != '\n')
            next();
        accept('\n');
    } while (start_md());

    if (ahead() == utf8::EoF)
        out_ = false;
    else
        md_fence();
}

Sym Lexer::sym() { return ast().sym(str_); }

} // namespace mim::ast
