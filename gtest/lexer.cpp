#include <string>

#include <gtest/gtest.h>

#include <mim/driver.h>

#include <mim/ast/ast.h>
#include <mim/ast/lexer.h>

using namespace std::literals;
using namespace mim;
using namespace mim::ast;

TEST(Lexer, Toks) {
    Driver driver;
    auto& w  = driver.world();
    auto ast = AST(w);
    Lexer lexer(ast, "{ } ( ) [ ] ‹ › « » : , . lam λ  23₀₁₂₃₄₅₆₇₈₉");

    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_brace_l));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_brace_r));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_paren_l));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_paren_r));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_brckt_l));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_brckt_r));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_angle_l));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_angle_r));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_quote_l));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_quote_r));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::T_colon));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::T_comma));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::T_dot));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::K_lam));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::T_lm));
    auto tok = lexer.lex();
    EXPECT_TRUE(tok.isa(Tok::Tag::L_i));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::EoF));
    EXPECT_EQ(tok.lit_i(), driver.world().lit_idx(123456789, 23));
}

TEST(Lexer, Annex) {
    Driver driver;
    auto& w  = driver.world();
    auto ast = AST(w);
    Lexer lexer(ast, "%plug.tag.sub %plug.tag.( %plug.a.b.c");

    auto sub = lexer.lex();
    EXPECT_TRUE(sub.isa(Tok::Tag::M_anx));
    EXPECT_EQ(sub.sym(), driver.sym("%plug.tag.sub"));

    auto tag = lexer.lex();
    EXPECT_TRUE(tag.isa(Tok::Tag::M_anx));
    EXPECT_EQ(tag.sym(), driver.sym("%plug.tag"));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::T_dot));
    EXPECT_TRUE(lexer.lex().isa(Tok::Tag::D_paren_l));
    EXPECT_EQ(ast.error().num_errors(), 0);

    auto deep = lexer.lex();
    EXPECT_EQ(ast.error().num_errors(), 1);
    EXPECT_EQ(deep.sym(), driver.sym("%plug.a")); // truncated to `%plugin.tag`
    ast.error().clear();
}

TEST(Lexer, Errors) {
    Driver driver;
    auto& w  = driver.world();
    auto ast = ast::AST(w);
    Lexer l1(ast, "asdf \xc0\xc0");
    l1.lex();
    l1.lex();
    EXPECT_GE(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str.starts_with("invalid UTF-8"));
    ast.error().clear();

    Lexer l2(ast, "foo \xaa");
    l2.lex();
    l2.lex();
    EXPECT_GE(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str.starts_with("invalid UTF-8"));
    ast.error().clear();

    Lexer l3(ast, "+");
    l3.lex();
    EXPECT_GE(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str.starts_with("stray"));
    ast.error().clear();

    Lexer l4(ast, "-");
    l4.lex();
    EXPECT_GE(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str.starts_with("stray"));
    ast.error().clear();
}

TEST(Lexer, Eof) {
    Driver driver;
    auto& w  = driver.world();
    auto ast = AST(w);
    Lexer lexer(ast, "");
    for (int i = 0; i < 10; i++)
        EXPECT_TRUE(lexer.lex().isa(Tok::Tag::EoF));
}

class Real : public testing::TestWithParam<int> {};

TEST_P(Real, sign) {
    Driver driver;
    auto& w  = driver.world();
    auto ast = AST(w);

    // clang-format off
    auto check = [&ast](std::string s, f64 r) {
        const auto sign = GetParam();
        switch (sign) {
            case 0: break;
            case 1: s.insert(0, "+"sv); break;
            case 2: s.insert(0, "-"sv); break;
            default: fe::unreachable();
        }

        Lexer lexer(ast, s);

        auto tag = lexer.lex();
        EXPECT_TRUE(tag.isa(Tok::Tag::L_f));
        EXPECT_EQ(std::bit_cast<f64>(tag.lit_u()), sign == 2 ? -r : r);
    };

    check(  "2e+3",   2e+3); check(  "2E3",   2E3);
    check( "2.e-3",  2.e-3); check( "2.E3",  2.E3); check( "2.3",  2.3);
    check( ".2e+3",  .2e+3); check( ".2E3",  .2E3); check( ".23",  .23);
    check("2.3e-4", 2.3e-4); check("2.3E4", 2.3E4); check("2.34", 2.34);

    check(  "0x2p+3",   0x2p+3); check(  "0x2P3",   0x2P3);
    check( "0x2.p-3",  0x2.p-3); check( "0x2.P3",  0x2.P3);
    check( "0x.2p+3",  0x.2p+3); check( "0x.2P3",  0x.2P3);
    check("0x2.3p-4", 0x2.3p-4); check("0x2.3P4", 0x2.3P4);
    // clang-format on

    Lexer l1(ast, "0x2.34");
    l1.lex();
    EXPECT_EQ(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str == "hexadecimal floating constants require an exponent"sv);
    ast.error().clear();

    Lexer l2(ast, "2.34e");
    l2.lex();
    EXPECT_EQ(ast.error().num_errors(), 1);
    EXPECT_TRUE(ast.error().msgs().front().str == "exponent has no digits");
    ast.error().clear();
}

INSTANTIATE_TEST_SUITE_P(Lexer, Real, testing::Range(0, 3));
