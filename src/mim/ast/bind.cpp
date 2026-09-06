#include "mim/ast/ast.h"

#include "family.h"

using namespace std::literals;

namespace mim::ast {

using Tag = Tok::Tag;

class DummyDecl : public Decl {
public:
    DummyDecl()
        : Decl(Loc()) {}

    void stream(fe::Tab&, std::ostream& os) const final { os << "<dummy>"; }
};

class Scopes {
public:
    Scopes(AST& ast)
        : ast_(ast)
        , dummy_(ast.ptr<DummyDecl>()) {
        push(); // root scope
    }

    AST& ast() const { return ast_; }
    Error& error() const { return ast().error(); }
    Scope& top() { return *scopes_.back().scope; }
    const Decl* dummy() const { return dummy_.get(); }

    /// An annex name is global: it lives in one flat table, independent of the module structure.
    static bool is_anx(Sym sym) { return sym && sym[0] == '%'; }

    void push() { scopes_.emplace_back(&owned_.emplace_back(), true); }
    void push(Scope& scope) { scopes_.emplace_back(&scope, false); }

    void pop() {
        assert(scopes_.size() > barrier_);
        if (scopes_.back().owned) owned_.pop_back();
        scopes_.pop_back();
    }

    /// A file must not see the scope of whoever imports it, so its Scope becomes the new lookup floor.
    size_t push_barrier(Scope& scope) {
        push(scope);
        return std::exchange(barrier_, scopes_.size() - 1);
    }

    void pop_barrier(size_t old) {
        barrier_ = old;
        pop();
    }

    const Decl* find(Dbg dbg, bool quiet = false) {
        if (dbg.is_anon()) return nullptr;

        if (is_anx(dbg.sym())) {
            if (auto decl = fe::lookup(anx_, dbg.sym())) return decl;
        } else {
            for (const auto& frame : scopes_ | std::views::drop(barrier_) | std::views::reverse)
                if (auto decl = fe::lookup(*frame.scope, dbg.sym())) return decl;
        }

        if (!quiet) {
            error().e(dbg.loc(), "identifier `{}` not found", dbg.sym());
            bind(dbg, dummy()); // put into scope to prevent further errors
        }
        return nullptr;
    }

    void bind(Dbg dbg, const Decl* decl, bool rebind = false, bool quiet = false) {
        if (dbg.is_anon()) return;

        auto& scope = is_anx(dbg.sym()) ? anx_ : top();
        if (rebind) {
            scope[dbg.sym()] = decl;
        } else if (auto [i, ins] = scope.try_emplace(dbg.sym(), decl); !ins) {
            auto prev = i->second;
            if (!quiet && !prev->isa<DummyDecl>()) // if prev stems from an error - don't complain
                error().e(dbg.loc(), "redeclaration of `{}`", dbg).n(prev->dbg().loc(), "previous declaration here");
        }
    }

private:
    struct Frame {
        Scope* scope;
        bool owned; ///< Whether Scopes::owned_ holds this Scope; a File's or ModDecl's Scope outlives Scopes.
    };

    AST& ast_;
    Ptr<DummyDecl> dummy_;
    std::deque<Scope> owned_;
    std::deque<Frame> scopes_;
    Scope anx_;
    size_t barrier_ = 0;
};

/*
 * File
 */

void File::bind(AST& ast) const {
    auto scopes = Scopes(ast);
    bind(scopes);
}

void File::bind(Scopes& s) const {
    if (bound_) return;
    bound_ = true;

    auto barrier = s.push_barrier(members());
    for (const auto& import : implicit_imports())
        import->bind(s);
    bind_decls(s);
    s.pop_barrier(barrier);
}

const Scope* Import::scope() const { return file() ? file()->scope() : nullptr; }

void Import::bind(Scopes& s) const {
    if (file()) file()->bind(s);

    // The same file may be imported more than once - as `-p foo` plus a `plugin foo;` directive, say.
    if (auto prev = s.find(dbg(), true))
        if (auto import = prev->isa<Import>(); import && import->file() == file()) return;

    s.bind(dbg(), this);
}

/*
 * Ptrn
 */

void ErrorPtrn::bind(Scopes&, bool, bool) const {}
void GrpPtrn::bind(Scopes& s, bool rebind, bool quiet) const { s.bind(dbg(), this, rebind, quiet); }

void IdPtrn::bind(Scopes& s, bool rebind, bool quiet) const {
    if (!quiet && type()) type()->bind(s);
    s.bind(dbg(), this, rebind, quiet);
}

void AliasPtrn::bind(Scopes& s, bool rebind, bool quiet) const {
    ptrn()->bind(s, rebind, quiet);
    s.bind(dbg(), this, rebind, quiet);
}

void TuplePtrn::bind(Scopes& s, bool rebind, bool quiet) const {
    for (const auto& ptrn : ptrns())
        ptrn->bind(s, rebind, quiet);
}

/*
 * Expr
 */

void PathExpr::bind(Scopes& s) const {
    decl_ = s.find(front());

    for (const auto& dbg : dbgs() | std::views::drop(1)) {
        if (!decl_) return;
        auto scope  = decl_->scope();
        auto member = scope ? fe::lookup(*scope, dbg.sym()) : nullptr;
        if (!member) {
            if (scope)
                s.error().e(dbg.loc(), "`{}` has no member `{}`", front().sym(), dbg.sym());
            else
                s.error().e(dbg.loc(), "`{}` is not a namespace", front().sym());
            decl_ = nullptr;
            return;
        }
        decl_ = member;
    }
}

// clang-format off
void TypeExpr   ::bind(Scopes& s) const { level()->bind(s); }
void RuleExpr   ::bind(Scopes& s) const { dom()->bind(s); }
void ErrorExpr  ::bind(Scopes&) const {}
void HoleExpr   ::bind(Scopes&) const {}
void PrimaryExpr::bind(Scopes&) const {}
// clang-format on

void LitExpr::bind(Scopes& s) const {
    if (type()) {
        type()->bind(s);
        if (ISA(tag(), C_LIT_TYPED)) s.error().e(type()->loc(), "a `{}` must not have a type annotation", tag());
    } else {
        if (tag() == Tag::L_f) s.error().e(loc(), "floating-point literal requires a type annotation");
    }
}

void DeclExpr::bind(Scopes& s) const {
    if (is_where())
        for (const auto& decl : decls() | std::views::reverse)
            decl->bind(s);
    else
        for (const auto& decl : decls())
            decl->bind(s);
    expr()->bind(s);
}

void ArrowExpr::bind(Scopes& s) const {
    dom()->bind(s);
    codom()->bind(s);
}

void UnionExpr::bind(Scopes& s) const {
    for (auto& type : types())
        type->bind(s);
}

void InjExpr::bind(Scopes& s) const {
    value()->bind(s);
    type()->bind(s);
}

void MatchExpr::Arm::bind(Scopes& s) const {
    s.push();
    ptrn()->bind(s, false, false);
    body()->bind(s);
    s.pop();
}

void MatchExpr::bind(Scopes& s) const {
    scrutinee()->bind(s);
    for (const auto& arm : arms())
        arm->bind(s);
}

void PiExpr::Dom::bind(Scopes& s, bool quiet) const {
    ptrn()->bind(s, false, quiet);
    if (ret()) ret()->bind(s, false, quiet);
}

void PiExpr::bind(Scopes& s) const {
    s.push();
    dom()->bind(s);
    if (codom()) {
        if (ISA(tag(), C_CN)) s.error().e(codom()->loc(), "a continuation must not have a codomain");
        codom()->bind(s);
    }
    s.pop();
}

void LamExpr::bind(Scopes& s) const {
    lam()->bind_decl(s);
    lam()->bind_body(s);
}

void AppExpr::bind(Scopes& s) const {
    callee()->bind(s);
    arg()->bind(s);
}

void RetExpr::bind(Scopes& s) const {
    callee()->bind(s);
    arg()->bind(s);
    ptrn()->bind(s, true, false);
    body()->bind(s);
}

void SigmaExpr::bind(Scopes& s) const {
    s.push();
    ptrn()->bind(s, false, false);
    s.pop();
}

void TupleExpr::bind(Scopes& s) const {
    for (const auto& elem : elems())
        elem->bind(s);
}

void SeqExpr::bind(Scopes& s) const {
    s.push();
    arity()->bind(s, false, false);
    body()->bind(s);
    s.pop();
}

void ExtractExpr::bind(Scopes& s) const {
    tuple()->bind(s);
    if (auto expr = std::get_if<Ptr<Expr>>(&index()))
        (*expr)->bind(s);
    else {
        auto dbg = std::get<Dbg>(index());
        decl_    = s.find(dbg, true);
    }
}

void InsertExpr::bind(Scopes& s) const {
    tuple()->bind(s);
    index()->bind(s);
    value()->bind(s);
}

void UniqExpr::bind(Scopes& s) const { inhabitant()->bind(s); }

/*
 * Decl
 */

void AxmDecl::Alias::bind(Scopes& s, const AxmDecl* axm) const {
    auto sym = s.ast().sym(axm->dbg().sym().str() + "."s + dbg().sym().str());
    full_    = Dbg(dbg().loc(), sym);
    s.bind(full_, this);
}

void AxmDecl::bind(Scopes& s) const {
    type()->bind(s);

    annex_ = s.ast().name2annex(dbg(), nullptr);

    if (annex_ && annex_->fresh) {
        annex_->normalizer = normalizer();
        annex_->pi         = type()->isa<PiExpr>() || type()->isa<ArrowExpr>();
    } else {
        auto pi = type()->isa<PiExpr>() || type()->isa<ArrowExpr>();
        if (annex_ && pi ^ *annex_->pi)
            s.error().e(dbg().loc(), "all declarations of annex `{}` must be function types if one of them is",
                        dbg().sym());

        if (annex_ && annex_->normalizer.sym() != normalizer().sym()) {
            auto l    = normalizer().loc() ? normalizer().loc() : loc().anew_end();
            auto& err = s.error().e(l, "normalizer mismatch for axm `{}`", dbg());
            if (auto norm = annex_->normalizer)
                err.n(norm.loc(), "previous normalizer `{}` declared here", norm);
            else
                err.n("initially no normalizer was specified");
        }
    }

    if (num_subs() == 0) {
        s.bind(dbg(), this);
    } else {
        if (auto old = s.find(dbg(), true)) {
            if (auto old_ax = old->isa<AxmDecl>()) {
                if (old_ax->num_subs() == 0) {
                    s.error()
                        .e(dbg().loc(), "axm `{}` was declared without subs and cannot be redeclared with subs", dbg())
                        .n(old_ax->dbg().loc(), "previous declaration here");
                }
            }
        }

        if (annex_) {
            offset_ = annex_->subs.size();
            for (const auto& aliases : subs())
                for (const auto& alias : aliases)
                    alias->bind(s, this);

            for (auto& sub : subs()) {
                auto& aliases = annex_->subs.emplace_back(std::deque<Sym>());
                for (const auto& alias : sub)
                    aliases.emplace_back(alias->dbg().sym());
            }
        }
    }
}

void LetDecl::bind(Scopes& s) const {
    s.push();
    value()->bind(s);
    s.pop();
    ptrn()->bind(s, true, false);

    if (auto id = ptrn()->isa<IdPtrn>()) annex_ = s.ast().name2annex(id->dbg(), &sub_);
}

void RecDecl::bind(Scopes& s) const {
    for (auto curr = this; curr; curr = curr->next())
        curr->bind_decl(s);
    for (auto curr = this; curr; curr = curr->next())
        curr->bind_body(s);
}

void RecDecl::bind_decl(Scopes& s) const {
    if (auto t = type()) t->bind(s);
    if (!type()->isa<HoleExpr>() && body()->isa<LamExpr>())
        s.error().w(type()->loc(), "type of recursive declaration ignored for function expression");

    if (!body()->isa<LamExpr>() && !body()->isa<PiExpr>() && !body()->isa<ArrowExpr>() && !body()->isa<SigmaExpr>())
        s.error().e(body()->loc(), "unsupported expression in a recursive declaration");

    s.bind(dbg(), this);
    annex_ = s.ast().name2annex(dbg(), &sub_);
}

void RecDecl::bind_body(Scopes& s) const { body()->bind(s); }

void LamDecl::Dom::bind(Scopes& s, bool quiet) const {
    PiExpr::Dom::bind(s, quiet);
    if (filter() && !quiet) filter()->bind(s);
}

void LamDecl::bind_decl(Scopes& s) const {
    s.push();
    for (size_t i = 0, e = num_doms(); i != e; ++i)
        dom(i)->bind(s);

    if (auto filter = doms().back()->filter()) {
        if (auto pe = filter->isa<PrimaryExpr>()) {
            if (pe->tag() == Tag::K_tt && ISA(tag(), C_DS))
                s.error().w(filter->loc(),
                            "`tt`-filter superfluous as the last curried function group of a `{}` receives a "
                            "`tt`-filter by default",
                            tag());
            if (pe->tag() == Tag::K_ff && !ISA(tag(), C_DS))
                s.error().w(filter->loc(),
                            "`ff`-filter superfluous as the last curried function group of a `{}` receives a "
                            "`ff`-filter by default",
                            tag());
        }
    }

    if (codom()) {
        if (ISA(tag(), C_CN)) s.error().e(codom()->loc(), "a continuation must not have a codomain");
        codom()->bind(s);
    }

    s.pop();
    s.bind(dbg(), this);
    annex_ = s.ast().name2annex(dbg(), &sub_);
}

void LamDecl::bind_body(Scopes& s) const {
    s.push();
    for (const auto& dom : doms())
        dom->bind(s, true);
    body()->bind(s);
    s.pop();
}

void CDecl::bind(Scopes& s) const {
    s.push();
    dom()->bind(s, false, false);
    s.pop(); // we don't allow codom to depent on dom
    if (codom()) codom()->bind(s);
    s.bind(dbg(), this);
}

void ModDecl::bind_decls(Scopes& s) const {
    for (const auto& decl : decls())
        decl->bind(s);
}

void ModDecl::bind(Scopes& s) const {
    s.push(members());
    bind_decls(s);
    s.pop();
    s.bind(dbg(), this);
}

void UseDecl::bind(Scopes& s) const {
    path()->bind(s);
    auto decl = path()->decl();
    if (!decl) return;

    auto scope = decl->scope();
    if (!scope) {
        s.error().e(path()->loc(), "`{}` is not a namespace", path()->back().sym());
        return;
    }

    // Quiet: a name already bound here wins, so `use` never shadows and never conflicts.
    for (const auto& [sym, decl] : *scope)
        s.bind(Dbg(path()->loc(), sym), decl, false, true);
}

void RuleDecl::bind(Scopes& s) const {
    s.push();
    var()->bind(s, true, false);
    lhs()->bind(s);
    rhs()->bind(s);
    guard()->bind(s);
    s.pop();
    s.bind(dbg(), this);
}

} // namespace mim::ast
