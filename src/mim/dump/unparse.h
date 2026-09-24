#pragma once

#include <string>

#include "mim/driver.h"

#include "mim/ast/ast.h"

#include "layout.h"

namespace mim::dump {

/// Builds the AST a Def reads back from - the dual of ast::Emitter; the Layout has decided everything beforehand.
class Unparser {
public:
    Unparser(ast::AST&, const Layout&);

    /// By name where the Layout binds one - unless @p header asks for what a Lam's header spells out inline.
    ast::Ptr<ast::Expr> expr(const Def*, bool header = false);
    ast::Ptr<ast::Expr> full(const Def*, bool header = false); ///< Spelled out with its operands.
    ast::Ptr<ast::Expr> block(const Block&);                   ///< `let`s and declarations before the tail.
    ast::Ptrs<ast::ValDecl> decls(const Block&);
    ast::Ptr<ast::ValDecl> use(const Driver::Imports::Entry&);

private:
    template<class T, class... Args>
    ast::Ptr<T> mk(Args&&... args) {
        return ast_.ptr<T>(Loc(), std::forward<Args>(args)...);
    }
    ast::Ptr<ast::Expr> full_(const Def*, bool header);
    ast::Ptr<ast::Expr> fallback(const Def*, bool header);
    ast::Ptr<ast::Expr> literal(const Lit*, bool header);
    ast::Ptr<ast::Expr> seq(const Seq*, bool header);
    ast::Ptr<ast::Expr> prim(ast::Tok::Tag);
    ast::Ptr<ast::Expr> raw(std::string_view);
    ast::Ptr<ast::Expr> path(Sym);
    ast::Ptr<ast::Expr> infix(ast::Ptr<ast::Expr>, ast::Tok::Tag, ast::Ptr<ast::Expr>);
    ast::Ptr<ast::Expr> app(ast::Ptr<ast::Expr>, ast::Ptr<ast::Expr>);
    ast::Ptr<ast::Expr> parens(ast::Ptr<ast::Expr>);
    ast::Ptr<ast::IdPtrn> id(Sym, ast::Ptr<ast::Expr> type);
    ast::Ptr<ast::TuplePtrn> tuple_ptrn(ast::Tok::Tag delim_l, ast::Ptrs<ast::Ptrn>);
    ast::Ptr<ast::Ptrn> ptrn(const Slot&, ast::Tok::Tag delim_l, bool header);
    ast::Ptr<ast::Ptrn> leaf(const Slot&, ast::Tok::Tag delim_l, bool header);
    ast::Ptr<ast::LamDecl> lam_decl(Lam* head, Dbg, ast::Mods, ast::Ptr<ast::RecDecl> next);
    ast::Ptr<ast::RecDecl> rec_decl(Def*, ast::Ptr<ast::RecDecl> next);
    ast::Ptr<ast::ValDecl> rule_decl(Rule*);
    void axms(const Item&, ast::Ptrs<ast::ValDecl>&);
    Sym axm_sym(const Axm*);

    ast::AST& ast_;
    const Layout& layout_;
    World& world_;
    std::string mod_;          ///< `<mod>.` while that `mod`'s Axm%s are declared - a `mod` does not see its own name.
    const Lam* ctx_ = nullptr; ///< The Lam whose header is being spelled; see Layout::resolve.
    DefSet active_;            ///< Mutables being spelled out - one that recurs has no spelling.
};

} // namespace mim::dump
