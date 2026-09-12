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
    Scope& top() { return scopes_.back().scope(); }
    const Decl* dummy() const { return dummy_.get(); }

    void push() { scopes_.emplace_back(); }
    void push(Scope& scope) { scopes_.emplace_back(scope); }

    void pop() {
        assert(scopes_.size() > barrier_);
        scopes_.pop_back();
    }

    /// A file must not see the scope of whoever imports it, so its Scope becomes the new lookup floor.
    size_t push_barrier(Scope& scope) {
        push(scope);
        auto old = std::exchange(barrier_, scopes_.size() - 1);
        mod_stack_.clear();
        return old;
    }

    void pop_barrier(size_t old) {
        barrier_ = old;
        pop();
    }

    /// @name Annex nesting
    /// Tracked by ModDecl::bind so AST::name2annex can derive `tag`/`sub` from lexical nesting.
    ///@{
    void push_mod(Sym name) { mod_stack_.emplace_back(name); }
    void pop_mod() { mod_stack_.pop_back(); }
    size_t mod_depth() const { return mod_stack_.size(); }
    Sym enclosing_mod() const { return mod_stack_.empty() ? Sym() : mod_stack_.back(); }
    ///@}

    const Decl* find(Dbg dbg, bool quiet = false) {
        if (dbg.is_anon()) return nullptr;

        for (auto& frame : scopes_ | std::views::drop(barrier_) | std::views::reverse)
            if (auto decl = fe::lookup(frame.scope(), dbg.sym())) return decl;

        if (!quiet) {
            auto& diag = error().e(dbg.loc(), "identifier `{}` not found", dbg.sym());
            // An infix operator only exists as whatever the user bound its escaped name to.
            if (dbg.sym().view().starts_with('`'))
                diag.n("an infix operator means whatever you bind its escaped name to");
            bind(dbg, dummy()); // put into scope to prevent further errors
        }
        return nullptr;
    }

    /// Diagnostic-only: is a module named @p sym reachable, shadowed by whatever `find` would actually return?
    /// Only `ModDecl`/`Import` ever yield a non-null Decl::scope, so this never confuses a value for a module.
    const Decl* find_shadowed_module(Sym sym) {
        for (auto& frame : scopes_ | std::views::drop(barrier_) | std::views::reverse)
            if (auto decl = fe::lookup(frame.scope(), sym); decl && decl->scope()) return decl;
        return nullptr;
    }

    void bind(Dbg dbg, const Decl* decl, bool rebind = false, bool quiet = false) {
        if (dbg.is_anon()) return;

        auto& scope = top();
        if (rebind) {
            scope[dbg.sym()] = decl;
        } else if (auto [i, ins] = scope.try_emplace(dbg.sym(), decl); !ins) {
            auto prev = i->second;
            if (!quiet && !prev->isa<DummyDecl>()) // if prev stems from an error - don't complain
                error().e(dbg.loc(), "redeclaration of `{}`", dbg).n(prev->dbg().loc(), "previous declaration here");
        } else if (!quiet && !decl->scope()) {
            if (auto mod = find_shadowed_module(dbg.sym()))
                error()
                    .w(dbg.loc(), "`{}` shadows a module of the same name", dbg)
                    .n(mod->dbg().loc(), "module declared here; a later `{}.member` would fail to resolve it",
                       dbg.sym());
        }
    }

private:
    /// Owns the Scope of an anonymous binder; a File's or ModDecl's Scope outlives Scopes and is borrowed.
    class Frame {
    public:
        Frame() = default;
        Frame(Scope& scope)
            : borrowed_(&scope) {}

        Scope& scope() { return borrowed_ ? *borrowed_ : own_; }

    private:
        Scope own_;
        Scope* borrowed_ = nullptr;
    };

    AST& ast_;
    Ptr<DummyDecl> dummy_;
    fe::Vector<Frame> scopes_;
    size_t barrier_ = 0;
    fe::Vector<Sym> mod_stack_;
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

void Path::bind(Scopes& s, bool quiet) const {
    decl_     = s.find(front(), quiet);
    auto prev = front();

    for (const auto& dbg : dbgs() | std::views::drop(1)) {
        if (!decl_) return;
        auto scope  = decl_->scope();
        auto member = scope ? fe::lookup(*scope, dbg.sym()) : nullptr;
        if (!member) {
            if (!quiet) {
                if (scope) {
                    s.error().e(dbg.loc(), "`{}` has no member `{}`", prev.sym(), dbg.sym());
                } else {
                    auto& err = s.error().e(prev.loc(), "`{}` is not a module", prev.sym());
                    if (auto mod = s.find_shadowed_module(prev.sym()))
                        err.n(mod->dbg().loc(), "a module `{}` exists here but is shadowed by the `{}` in scope",
                              prev.sym(), prev.sym());
                }
            }
            decl_ = nullptr;
            return;
        }
        decl_ = member;
        prev  = dbg;

        // A dotted path always crosses into decl_'s enclosing mod from outside: `priv` blocks it.
        if (decl_->vis() == Vis::Priv) {
            if (!quiet) s.error().e(dbg.loc(), "`{}` is private to its enclosing `mod`", dbg.sym());
            decl_ = nullptr;
            return;
        }
    }
}

// clang-format off
void PathExpr   ::bind(Scopes& s) const { path()->bind(s); }
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

void InfixExpr::bind(Scopes& s) const {
    if (callee()) callee()->bind(s);
    lhs()->bind(s);
    // `t#x` may name a field of `t`'s Sigma rather than anything in scope; InfixExpr::emit_ resolves that.
    if (auto path = op().isa(Tag::T_extract) ? rhs()->isa<PathExpr>() : nullptr)
        path->path()->bind(s, true);
    else
        rhs()->bind(s);
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

void UniqExpr::bind(Scopes& s) const { inhabitant()->bind(s); }

/*
 * Decl
 */

AnnexInfo* AST::name2annex(Scopes& s, Dbg dbg, sub_t* sub_id) {
    if (!dbg) return nullptr;

    auto depth = s.mod_depth();
    if (depth > 1) {
        error().e(dbg.loc(), "`{}` sits {} `mod` levels deep; an `anx` declaration may nest at most one", dbg, depth);
        return nullptr;
    }

    auto plugin_s = dbg.loc().src ? sym(fs::path(dbg.loc().src->path()).stem().string()) : sym_error();
    auto tag_s    = depth == 0 ? dbg.sym() : s.enclosing_mod();
    Sym sub_s     = depth == 0 ? Sym() : dbg.sym();

    auto& sym2annex = plugin2sym2annex_[plugin_s];
    auto tag_id     = sym2annex.size();

    if (plugin_s == sym_error()) error().e(dbg.loc(), "plugin name `{}` is reserved", dbg);
    if (tag_id > std::numeric_limits<tag_t>::max())
        error().e(dbg.loc(), "exceeded maximum number of annexes in current plugin");

    if (!Annex::mangle(plugin_s)) {
        error().e(dbg.loc(), "invalid annex name `{}`", dbg);
        plugin_s = sym_error();
    }

    auto [i, fresh] = sym2annex.try_emplace(tag_s, AnnexInfo{plugin_s, tag_s, (tag_t)tag_id});
    auto annex      = &i->second;

    if (sub_s) {
        if (sub_id) {
            *sub_id       = annex->subs.size();
            auto& aliases = annex->subs.emplace_back();
            aliases.emplace_back(sub_s);
        } else {
            error().e(dbg.loc(), "annex `{}` must not have a subtag", dbg);
        }
    }

    if (!fresh) annex->fresh = false;
    return annex;
}

void AxmDecl::bind(Scopes& s) const {
    type()->bind(s);

    annex_ = s.ast().name2annex(s, dbg(), &sub_);

    if (annex_ && annex_->fresh) {
        annex_->normalizer = normalizer();
        annex_->pi         = type()->isa<PiExpr>() || InfixExpr::isa_op(Tag::T_arrow, type());
    } else if (annex_) {
        auto pi = type()->isa<PiExpr>() || InfixExpr::isa_op(Tag::T_arrow, type());
        if (pi ^ *annex_->pi)
            s.error().e(dbg().loc(),
                        "all declarations of annex `{}` must be function types if one of them is (they share one "
                        "annex tag - via mod-nesting or a `tag.(...)` family - and must agree in shape)",
                        dbg().sym());

        if (annex_->normalizer.sym() != normalizer().sym()) {
            auto l    = normalizer().loc() ? normalizer().loc() : loc().anew_end();
            auto& err = s.error().e(l, "normalizer mismatch for axm `{}`", dbg());
            if (auto norm = annex_->normalizer)
                err.n(norm.loc(), "previous normalizer `{}` declared here", norm);
            else
                err.n("initially no normalizer was specified");
        }
    }

    s.bind(dbg(), this);
}

void AxmDecl::Sibling::bind(Scopes& s) const {
    annex_ = s.ast().name2annex(s, dbg(), &sub_);
    s.bind(dbg(), this);
}

void AliasDecl::bind(Scopes& s) const {
    path()->bind(s);
    s.bind(dbg(), this);

    auto target = path()->decl();
    if (!target) return;

    if (target->isa<AliasDecl>()) {
        s.error().e(loc(), "`{}` aliases `{}`, which is itself an alias; alias chains are not supported", dbg(),
                    path()->back());
        return;
    }

    std::tie(annex_, sub_) = target->annex_sub();
    if (!annex_) {
        s.error().e(loc(), "`{}` must alias a compiler-exposed (`anx`) declaration", dbg());
        return;
    }

    // An ungrouped target (the common case) never allocated its own sub-group; seed one now,
    // named after the target itself, so this alias has a slot to share.
    if (sub_ >= annex_->subs.size()) {
        assert(sub_ == annex_->subs.size());
        annex_->subs.emplace_back(std::deque<Sym>{target->dbg().sym()});
    }
    annex_->subs[sub_].emplace_back(dbg().sym());
}

void LetDecl::bind(Scopes& s) const {
    s.push();
    value()->bind(s);
    s.pop();
    ptrn()->bind(s, true, false);

    if (auto id = ptrn()->isa<IdPtrn>()) {
        id->vis_ = vis();
        id->anx_ = is_anx();
        if (is_anx()) id->annex_ = s.ast().name2annex(s, id->dbg(), &id->sub_);
    }
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

    if (!body()->isa<LamExpr>() && !body()->isa<PiExpr>() && !InfixExpr::isa_op(Tag::T_arrow, body())
        && !body()->isa<SigmaExpr>())
        s.error().e(body()->loc(), "unsupported expression in a recursive declaration");

    s.bind(dbg(), this);
    if (is_anx()) annex_ = s.ast().name2annex(s, dbg(), &sub_);
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
    if (is_anx()) annex_ = s.ast().name2annex(s, dbg(), &sub_);
}

void LamDecl::bind_body(Scopes& s) const {
    s.push();
    for (const auto& dom : doms())
        dom->bind(s, true);
    if (body()) body()->bind(s);
    s.pop();
}

void ModDecl::bind_decls(Scopes& s) const {
    for (const auto& decl : decls())
        decl->bind(s);
}

void ModDecl::bind(Scopes& s) const {
    s.push(members());
    s.push_mod(dbg().sym());
    bind_decls(s);
    s.pop_mod();
    s.pop();
    s.bind(dbg(), this);
}

const Scope* UseDecl::module(Scopes& s) const {
    if (is_import()) {
        if (!file()) return nullptr;
        file()->bind(s);
        return file()->scope();
    }

    path()->bind(s);
    auto decl = path()->decl();
    if (!decl) return nullptr;

    auto scope = decl->scope();
    if (!scope) s.error().e(path()->loc(), "`{}` is not a module", path()->back().sym());
    return scope;
}

void UseDecl::bind(Scopes& s) const {
    auto mod = module(s);
    if (!mod) return;

    if (is_splice()) {
        // Quiet: a name already bound here wins, so a splice never shadows and never conflicts.
        for (const auto& [sym, decl] : *mod)
            if (decl->vis() == Vis::Pub) s.bind(Dbg(loc(), sym), decl, false, true);
        return;
    }

    // The same file may be imported more than once - as `-p foo` plus a `plugin foo;` directive, say.
    if (file())
        if (auto prev = s.find(dbg(), true))
            if (auto use = prev->isa<UseDecl>(); use && use->file() == file()) return;

    scope_ = mod;
    s.bind(dbg(), this);
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
