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
    const File* import(std::string_view sv, Tok::Tag tag = Tok::Tag::K_import) {
        return import({Loc(), driver().sym(sv)}, false, tag, nullptr);
    }
    /// @p is_path selects the `import "some/path.mim"` form over the search-path lookup by name.
    /// @p record is `false` for the compilation root: it is not a directive that a dump should reproduce.
    const File*
    import(Dbg, bool is_path, Tok::Tag tag = Tok::Tag::K_import, std::ostream* md = nullptr, bool record = true);
    const File* import(const fe::Src&, std::ostream* md = nullptr, Loc = {});
    /// Imports the @p plugins the Driver was told about via `-p` as anonymous, unaliased UseDecl%s.
    Ptrs<UseDecl> import_plugins(fe::View<std::string> plugins, Tok::Tag);
    /// Slurps @p is, registers it in Driver::src under @p path, and parses it.
    const File* import(std::istream& is, fs::path path, Loc = {}, std::ostream* md = nullptr);
    const File* import_main(std::string_view input, fe::View<std::string> plugins, std::ostream* md = nullptr);

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
    Ptr<File> parse_file();
    Dbg parse_id(std::string_view ctxt = {});
    Path parse_path(std::string_view ctxt = {});
    Ptr<UseDecl> parse_import_or_plugin();
    Ptr<Expr> parse_type_ascr(std::string_view ctxt = {});

    /// Directory of the file currently being parsed; empty if its Loc%s have no fe::Src.
    fs::path curr_dir() const { return curr_.src ? curr_.src->path().parent_path() : fs::path(); }
    Ptr<Expr> path_expr(Dbg dbg) { return ptr<PathExpr>(Path(dbg)); }

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
    Ptr<Expr> parse_infix_expr(Tracker, Ptr<Expr>&& lhs, Prec = Prec::Bot, std::string_view ctxt = {});

    /// The `` `op `` a MIM_INFIX_SUGAR operator desugars to; `nullptr` for a MIM_INFIX_CORE one.
    Ptr<Expr> sugar_callee(Tok op);
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

    /// Parses any combination of `priv`/`pub`/`extern`/`anx` modifier tokens, in any order.
    /// Only rejects a modifier being repeated (`priv priv`, `extern extern`, ...);
    /// whether a given combination makes sense for the decl that follows is up to that decl's own parser.
    Mods parse_modifiers();
    /// Errors if @p mods sets `extern`, for decl kinds that don't support it (`let`/bare `rec`):
    /// currently only a function declaration (`lam`/`con`/`fun`, see Parser::parse_lam_decl) may be `extern`.
    /// The default-`Vis` nudge itself lives in Mods::default_vis, resolved lazily by ValDecl::vis.
    /// `mod` doesn't call this at all: as pure AST grouping it supports neither `extern` nor `anx`
    /// (see Parser::parse_mod_decl).
    void check_no_extern(const Mods&, std::string_view entity);
    void parse_axm_decl(Tracker, Mods, Ptrs<ValDecl>&);
    /// Parses the `(tag_0 [= alias]*, ...): type[, normalizer[, curry[, trip]]]` tail shared by a bare
    /// `axm (...)` group and the `axm tag.(...)` family-sugar; each Dbgs is one tag's `[primary, alias, ...]`.
    Ptrs<ValDecl> parse_axm_group(Vis);
    /// The `: type[, normalizer[, curry[, trip]]]` tail shared by a plain `axm` and Parser::parse_axm_group.
    std::tuple<Ptr<Expr>, Dbg, Tok, Tok> parse_axm_tail();
    Ptr<ValDecl> parse_alias_decl(Tracker, Mods);
    Ptr<ValDecl> parse_let_decl(Tracker, Mods);
    Ptr<ValDecl> parse_mod_decl(Tracker, Mods);
    Ptr<ValDecl> parse_use_decl(Tracker, Mods);
    Ptr<ValDecl> parse_rule_decl();
    Ptr<LamDecl> parse_lam_decl(Tracker, Mods);
    Ptr<RecDecl> parse_rec_decl(Tracker, bool first, Mods);
    Ptr<RecDecl> parse_and_decl();
    ///@}

    AST& ast_;
    Lexer* lexer_ = nullptr;

    friend Super;
};

} // namespace mim::ast
