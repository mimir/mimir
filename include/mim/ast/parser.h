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
    Driver& driver() { return ast().driver(); } ///< fe::Parser's default diagnostics go to its Driver::error.
    Ptr<Module> import(std::string_view sv, Tok::Tag tag = Tok::Tag::K_import) {
        return import({Loc(), driver().sym(sv)}, nullptr, tag);
    }
    Ptr<Module> import(Dbg, std::ostream* md = nullptr, Tok::Tag tag = Tok::Tag::K_import);
    Ptr<Module> import(const fe::Src&, std::ostream* md = nullptr);
    /// Slurps @p is, registers it in Driver::src under @p path, and parses it.
    Ptr<Module> import(std::istream& is, fs::path path, Loc = {}, std::ostream* md = nullptr);
    Ptr<Module> import_main(std::string_view input, fe::View<std::string> plugins, std::ostream* md = nullptr);

private:
    template<class T, class... Args>
    auto ptr(Args&&... args) {
        return ast_.ptr<const T>(std::forward<Args>(args)...);
    }

    /// Empty Loc right after the last consumed token - where a node sits that is *missing* rather than wrong.
    Loc missing() const { return curr_.anew_end(); }
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
    void parse_list(std::string_view ctxt, Tok::Tag delim_l, F f, Tok::Tag sep = Tok::Tag::T_comma) {
        expect(delim_l, ctxt);
        auto delim_r = Tok::delim_l2r(delim_l);
        auto _       = this->anchor(delim_r);
        do {
            recover(ctxt);
            if (ahead().isa(delim_r)) break;
            f();
            recover(ctxt);
        } while (accept(sep));
        expect(delim_r, "closing delimiter of a {}", ctxt);
    }

    /// Discard all closing delimiters that no enclosing context is waiting for.
    void recover(std::string_view ctxt) { Super::recover(Tok::is_delim_r, ctxt); }
    ///@}

    /// @name parse exprs
    ///@{
    Ptr<Expr> parse_expr(std::string_view ctxt, Prec = Prec::Bot);

    /// As above but builds @p ctxt via std::format.
    template<class... Args>
    Ptr<Expr> parse_expr(std::format_string<Args...> fmt, Args&&... args) {
        return parse_expr(std::format(fmt, std::forward<Args>(args)...));
    }

    /// As above but with @p prec.
    template<class... Args>
    Ptr<Expr> parse_expr(Prec prec, std::format_string<Args...> fmt, Args&&... args) {
        return parse_expr(std::format(fmt, std::forward<Args>(args)...), prec);
    }
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

    /// @name parse ptrns
    ///@{

    /// A pattern `p` binds a name, whereas a binder `b` (PtrnStyle::brckt) also accepts a bare type expression.
    struct PtrnStyle {
        bool brckt    = false;
        bool implicit = false; ///< Also accept `{b, ..., b}`.
    };

    Ptr<Ptrn> parse_ptrn(PtrnStyle, std::string_view ctxt, Prec = Prec::Bot);

    /// As above but builds @p ctxt via std::format.
    template<class... Args>
    Ptr<Ptrn> parse_ptrn(PtrnStyle style, Prec prec, std::format_string<Args...> fmt, Args&&... args) {
        return parse_ptrn(style, std::format(fmt, std::forward<Args>(args)...), prec);
    }
    Ptr<Ptrn> parse_ptrn_(PtrnStyle, std::string_view ctxt, Prec = Prec::Bot);
    Ptr<TuplePtrn> parse_tuple_ptrn(PtrnStyle);

    /// The empty Sym - as opposed to `_` - is what lets Ptrn::to_expr turn this binder back into an expression.
    Ptr<IdPtrn> anon_ptrn(Loc loc, Ptr<Expr>&& type) {
        return ptr<IdPtrn>(loc, Dbg(loc.anew_begin()), std::move(type));
    }
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

    AST& ast_;
    Lexer* lexer_ = nullptr;

    friend Super;
};

} // namespace mim::ast
