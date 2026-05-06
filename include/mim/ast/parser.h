#pragma once

#include <fe/parser.h>

#include "mim/ast/ast.h"
#include "mim/ast/lexer.h"

namespace mim::ast {

constexpr size_t Look_Ahead = 2;

/// Parses Mim code as AST.
///
/// The logic behind the various parse methods is as follows:
/// 1. The `parse_*` method does **not** have a `std::string_view ctxt` parameter:
///
///     It's the **caller's responsibility** to first make appropriate
///     [FIRST/FOLLOW](https://www.cs.uaf.edu/~cs331/notes/FirstFollow.pdf) checks.
///     Otherwise, an assertion will be triggered in the case of a syntax error.
///
/// 2. The `parse_*` method does have a `std::string_view ctxt` parameter:
///
///      The **called method** checks this and spits out an appropriate error message using `ctxt` in the case of a
///      syntax error.
///
/// 3. The `parse_*` method does have a `std::string_view ctxt = {}` parameter **with default argument**:
///
///      * If default argument is **elided** we have the same behavior as in 1.
///      * If default argument is **provided** we have the same behavior as in 2.
class Parser : public fe::Parser<Tok, Tok::Tag, Look_Ahead, Parser> {
    using Super = fe::Parser<Tok, Tok::Tag, Look_Ahead, Parser>;

public:
    Parser(AST& ast)
        : ast_(ast) {}

    AST& ast() { return ast_; }
    Driver& driver() { return ast().driver(); }
    Ptr<Module> import(std::string_view sv, Tok::Tag tag = Tok::Tag::K_import) {
        return import({Loc(), driver().sym(sv)}, nullptr, tag);
    }
    Ptr<Module> import(Dbg, std::ostream* md = nullptr, Tok::Tag tag = Tok::Tag::K_import);
    Ptr<Module> import(std::istream&, Loc = {}, const fs::path* = nullptr, std::ostream* md = nullptr);
    Ptr<Module> plugin(Dbg);
    Ptr<Module> plugin(const std::string& s) { return plugin({Loc(), driver().sym(s)}); }
    Ptr<Module> plugin(const char* name) { return plugin({Loc(), driver().sym(name)}); }

private:
    template<class T, class... Args>
    auto ptr(Args&&... args) {
        return ast_.ptr<const T>(std::forward<Args>(args)...);
    }

    Dbg anon() const { return {ahead().loc().anew_begin(), ast_.sym_anon()}; }
    Dbg dbg(const Tracker& tracker, Sym sym) const { return {tracker.loc(), sym}; }
    Lexer& lexer() { return *lexer_; }

    /// @name parse misc
    ///@{
    Ptr<Module> parse_module();
    Dbg parse_id(std::string_view ctxt = {});
    std::pair<Annex&, bool> parse_annex(std::string_view ctxt = {});
    Dbg parse_name(std::string_view ctxt = {});
    Ptr<Import> parse_import_or_plugin();
    Ptr<Import> parse_plugin();
    Ptr<Expr> parse_type_ascr(std::string_view ctxt = {});

    template<class F>
    void parse_list(std::string ctxt, Tok::Tag delim_l, F f, Tok::Tag sep = Tok::Tag::T_comma) {
        expect(delim_l, ctxt);
        auto delim_r = Tok::delim_l2r(delim_l);
        if (!ahead().isa(delim_r)) {
            do {
                f();
            } while (accept(sep) && !ahead().isa(delim_r));
        }
        expect(delim_r, std::string("closing delimiter of a ") + ctxt);
    }
    ///@}

    /// @name parse exprs
    ///@{
    Ptr<Expr> parse_expr(std::string_view ctxt, Prec = Prec::Bot);
    Ptr<Expr> parse_primary_expr(std::string_view ctxt);
    Ptr<Expr> parse_infix_expr(Tracker, Ptr<Expr>&& lhs, Prec = Prec::Bot);
    ///@}

    /// @name parse primary exprs
    ///@{
    Ptr<Expr> parse_decl_expr();
    Ptr<Expr> parse_lit_expr();
    Ptr<Expr> parse_extremum_expr();
    Ptr<Expr> parse_type_expr();
    Ptr<Expr> parse_rule_expr();
    Ptr<Expr> parse_ret_expr();
    Ptr<Expr> parse_pi_expr();
    Ptr<Expr> parse_pi_expr(Ptr<Ptrn>&&);
    Ptr<Expr> parse_lam_expr();
    Ptr<Expr> parse_seq_expr();
    Ptr<Expr> parse_sigma_expr();
    Ptr<Expr> parse_tuple_expr();
    Ptr<Expr> parse_insert_expr();
    Ptr<Expr> parse_uniq_expr();
    Ptr<Expr> parse_match_expr();
    ///@}

    enum PtrnStyle {
        Style_Bit   = 0b001,
        Brckt_Style = 0b001,
        Paren_Style = 0b000,
        Implicit    = 0b010,
    };

    /// @name parse ptrns
    ///@{
    static bool is_paren_style(int style) { return (style & Style_Bit) == Paren_Style; }
    static bool is_brket_style(int style) { return (style & Style_Bit) == Brckt_Style; }
    static bool is_implicit(int style) { return (style & Implicit); }
    Ptr<Ptrn> parse_ptrn(int style, std::string_view ctxt, Prec = Prec::Bot);
    Ptr<Ptrn> parse_ptrn_(int style, std::string_view ctxt, Prec = Prec::Bot);
    Ptr<TuplePtrn> parse_tuple_ptrn(int style);
    ///@}

    /// @name parse decls
    ///@{
    /// If @p ctxt ...
    /// * ... empty: **Only** decls are parsed. @returns `nullptr`
    /// * ... **non**-empty: Decls are parsed, then an expression. @returns expression.
    Ptrs<ValDecl> parse_decls();
    Ptr<ValDecl> parse_axm_decl();
    Ptr<ValDecl> parse_let_decl();
    Ptr<ValDecl> parse_c_decl();
    Ptr<ValDecl> parse_rule_decl();
    Ptr<LamDecl> parse_lam_decl();
    Ptr<RecDecl> parse_rec_decl(bool first);
    Ptr<RecDecl> parse_and_decl();
    ///@}

    /// @name error messages
    ///@{
    /// Issue an error message of the form:
    /// "expected \<what\>, got '\<tok>\' while parsing \<ctxt\>"
    void syntax_err(std::string_view what, const Tok& tok, std::string_view ctxt) {
        ast().error(tok.loc(), "expected {}, got '{}' while parsing {}", what, tok, ctxt);
    }

    /// Same above but uses @p ahead() as @p tok.
    void syntax_err(std::string_view what, std::string_view ctxt) { syntax_err(what, ahead(), ctxt); }

    void syntax_err(Tok::Tag tag, std::string_view ctxt) {
        std::string msg("'");
        msg.append(Tok::tag2str(tag)).append("'");
        syntax_err(msg, ctxt);
    }

    using Super::expect;
    template<class... Args>
    Tok expect(Tok::Tag tag, const char* f, Args&&... args) {
        std::ostringstream oss;
        print(oss, f, std::forward<Args>(args)...);
        return Super::expect(tag, oss.str());
    }
    ///@}

    AST& ast_;
    Lexer* lexer_ = nullptr;

    friend Super;
};

} // namespace mim::ast
