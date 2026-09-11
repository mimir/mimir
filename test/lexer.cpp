#include <string>

#include <doctest/doctest.h>

#include <mim/driver.h>

#include <mim/ast/lexer.h>

using namespace std::literals;
using namespace mim;
using namespace mim::ast;

TEST_CASE("Lexer") {
    Driver drv;

    SUBCASE("delimiters, keywords and a subscripted literal") {
        Lexer lexer(drv, "{ } ( ) [ ] ‹ › « » : , . lam λ  23₀₁₂₃₄₅₆₇₈₉");

        CHECK(lexer.lex().isa(Tok::Tag::D_brace_l));
        CHECK(lexer.lex().isa(Tok::Tag::D_brace_r));
        CHECK(lexer.lex().isa(Tok::Tag::D_paren_l));
        CHECK(lexer.lex().isa(Tok::Tag::D_paren_r));
        CHECK(lexer.lex().isa(Tok::Tag::D_brckt_l));
        CHECK(lexer.lex().isa(Tok::Tag::D_brckt_r));
        CHECK(lexer.lex().isa(Tok::Tag::D_angle_l));
        CHECK(lexer.lex().isa(Tok::Tag::D_angle_r));
        CHECK(lexer.lex().isa(Tok::Tag::D_quote_l));
        CHECK(lexer.lex().isa(Tok::Tag::D_quote_r));
        CHECK(lexer.lex().isa(Tok::Tag::T_colon));
        CHECK(lexer.lex().isa(Tok::Tag::T_comma));
        CHECK(lexer.lex().isa(Tok::Tag::T_dot));
        CHECK(lexer.lex().isa(Tok::Tag::K_lam));
        CHECK(lexer.lex().isa(Tok::Tag::T_lm));

        auto tok = lexer.lex();
        CHECK(tok.isa(Tok::Tag::L_i));
        CHECK(lexer.lex().isa(Tok::Tag::EoF));
        CHECK(tok.lit_i() == std::pair(u64(123456789), u64(23)));
    }

    SUBCASE("infix operators and their escaped names") {
        Lexer lexer(drv, "+ - * / % == != < <= > >= << >> -> `+ `- `* `/ `% `== `!= `< `<= `> `>= `<< `>>");

        CHECK(lexer.lex().isa(Tok::Tag::T_add));
        CHECK(lexer.lex().isa(Tok::Tag::T_sub));
        CHECK(lexer.lex().isa(Tok::Tag::T_star));
        CHECK(lexer.lex().isa(Tok::Tag::T_div));
        CHECK(lexer.lex().isa(Tok::Tag::T_rem));
        CHECK(lexer.lex().isa(Tok::Tag::T_eq));
        CHECK(lexer.lex().isa(Tok::Tag::T_ne));
        CHECK(lexer.lex().isa(Tok::Tag::T_lt));
        CHECK(lexer.lex().isa(Tok::Tag::T_le));
        CHECK(lexer.lex().isa(Tok::Tag::T_gt));
        CHECK(lexer.lex().isa(Tok::Tag::T_ge));
        CHECK(lexer.lex().isa(Tok::Tag::T_shl));
        CHECK(lexer.lex().isa(Tok::Tag::T_shr));
        CHECK(lexer.lex().isa(Tok::Tag::T_arrow));

        for (auto op : {"`+"sv, "`-"sv, "`*"sv, "`/"sv, "`%"sv, "`=="sv, "`!="sv, "`<"sv, "`<="sv, "`>"sv, "`>="sv,
                        "`<<"sv, "`>>"sv}) {
            auto tok = lexer.lex();
            CHECK(tok.isa(Tok::Tag::M_id));
            CHECK(tok.sym() == op);
        }

        CHECK(lexer.lex().isa(Tok::Tag::EoF));
        CHECK(drv.error().num_errors() == 0);
    }

    SUBCASE("EoF is sticky") {
        Lexer lexer(drv, "");
        for (int i = 0; i < 10; i++)
            CHECK(lexer.lex().isa(Tok::Tag::EoF));
    }

    SUBCASE("errors") {
        auto check = [&drv](std::string_view buf, std::string_view prefix) {
            CAPTURE(buf);
            Lexer lexer(drv, buf);
            lexer.lex();
            lexer.lex();
            CHECK(drv.error().num_errors() >= 1);
            CHECK(drv.error().msgs().front().str.starts_with(prefix));
            drv.error().clear();
        };

        check("asdf \xc0\xc0", "invalid UTF-8");
        check("foo \xaa", "invalid UTF-8");
        check("`", "expected one of");
    }
}

TEST_CASE("Lexer: floating-point literals") {
    Driver drv;

    // A sign is an infix operator to the Lexer; Parser::parse_lit_expr applies it to the literal.
    auto check = [&drv](std::string s, f64 r) {
        CAPTURE(s);

        Lexer lexer(drv, s);
        auto tok = lexer.lex();
        CHECK(tok.isa(Tok::Tag::L_f));
        CHECK(std::bit_cast<f64>(tok.lit_u()) == r);
    };

    // clang-format off
    check(  "2e+3",   2e+3); check(  "2E3",   2E3);
    check( "2.e-3",  2.e-3); check( "2.E3",  2.E3); check( "2.3",  2.3);
    check( ".2e+3",  .2e+3); check( ".2E3",  .2E3); check( ".23",  .23);
    check("2.3e-4", 2.3e-4); check("2.3E4", 2.3E4); check("2.34", 2.34);

    check(  "0x2p+3",   0x2p+3); check(  "0x2P3",   0x2P3);
    check( "0x2.p-3",  0x2.p-3); check( "0x2.P3",  0x2.P3);
    check( "0x.2p+3",  0x.2p+3); check( "0x.2P3",  0x.2P3);
    check("0x2.3p-4", 0x2.3p-4); check("0x2.3P4", 0x2.3P4);
    // clang-format on
}

TEST_CASE("Lexer: malformed floating-point literals") {
    Driver drv;

    auto check = [&drv](std::string_view buf, std::string_view msg) {
        CAPTURE(buf);
        Lexer lexer(drv, buf);
        lexer.lex();
        CHECK(drv.error().num_errors() == 1);
        CHECK(drv.error().msgs().front().str == msg);
        drv.error().clear();
    };

    check("0x2.34", "hexadecimal floating constants require an exponent");
    check("2.34e", "exponent has no digits");
}

TEST_CASE("Lexer: escape round-trips through a string literal") {
    Driver drv;

    auto check = [&drv](std::string_view raw) {
        CAPTURE(raw);
        auto buf = "\""s + Lexer::escape(raw) + "\"";
        Lexer lexer(drv, buf);
        auto tok = lexer.lex();
        CHECK(drv.error().num_errors() == 0);
        CHECK(tok.isa(Tok::Tag::L_str));
        CHECK(tok.sym() == raw);
    };

    check("plain/path.mim");
    check("we\"ird.mim");
    check("back\\slash.mim");
    check("tab\there\nand\r\0more"sv);
}
