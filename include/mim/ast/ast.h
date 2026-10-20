#pragma once

#include <deque>
#include <memory>
#include <tuple>

#include <fe/arena.h>
#include <fe/assert.h>
#include <fe/cast.h>
#include <fe/vla.h>

#include "mim/driver.h"

#include "mim/ast/tok.h"

namespace mim::ast {

class Decl;
class Expr;
class LamDecl;
class File;
class Scopes;
class Emitter;

/// Nodes live in the AST's Arena and are never destroyed, so this merely points at one.
template<class T>
using Ptr = fe::Arena::Ref<const T>;

/// @name Ptrs/Dbgs
/// Scratch buffers the Parser fills before it creates a node; a node keeps its own lists right
/// behind itself - see fe::VLA - and hands them out as a fe::View.
///@{
template<class T>
using Ptrs = fe::Vector<Ptr<T>>;
using Dbgs = fe::Vector<Dbg>;
///@}

/// Visibility tier of a ValDecl.
/// `Priv` is only visible inside its enclosing `mod`; `Pub` is visible via a path/`use` from outside.
/// Orthogonal to this, a ValDecl may independently be `extern` (Decl::is_extern) and/or `anx` (Decl::is_anx);
/// either one nudges the default visibility to `Pub` unless `priv` is given explicitly.
enum class Vis { Priv, Pub };

/// One name in a Scope: the Decl it introduces and the Vis of *this* binding.
/// A splicing UseDecl re-binds someone else's Decl under its own Vis, so Vis belongs here and not to the Decl.
struct Bind {
    const Decl* decl;
    Vis vis;
};

using Scope = fe::SymMap<Bind>; ///< Maps a name to the Binding introducing it.

/// Raw, unvalidated combination of `priv`/`pub`/`extern`/`anx` modifiers written before a declaration.
/// Parser::parse_modifiers only rejects a modifier being repeated (`priv priv`, `extern extern`, ...);
/// whether a given combination makes sense for the decl that follows is up to that decl's own parser.
struct Mods {
    std::optional<Vis> vis = {};
    bool is_extern         = false;
    bool is_anx            = false;

    /// `extern`/`anx` nudge the default visibility to `Pub` unless `priv` is given explicitly.
    Vis default_vis() const { return (is_extern || is_anx) ? Vis::Pub : Vis::Priv; }
    Vis resolved_vis() const { return vis.value_or(default_vis()); }
    bool any() const { return vis || is_extern || is_anx; }
    /// @p group's modifiers as defaults, so an explicit `priv`/`pub` here overrides the group's.
    Mods inherit(const Mods& group) const {
        return {vis ? vis : group.vis, is_extern || group.is_extern, is_anx || group.is_anx};
    }
};

/// Bookkeeping of an annex introduced by an AxmDecl.
struct AnnexInfo {
    AnnexInfo(Sym sym_plugin, Sym sym_tag, tag_t id_tag)
        : sym{sym_plugin, sym_tag}
        , id{id_tag} {}

    /// The mangled `plugin` part of the flags.
    /// Derived from sym.plugin which is guaranteed mangleable by the time an AnnexInfo exists.
    plugin_t plugin_id() const { return *Annex::mangle(sym.plugin); }
    /// The base flags (`plugin` + `tag`, no `sub`).
    flags_t base() const { return Annex::flags(plugin_id(), id.tag); }
    /// Fully-qualified `plugin.tag[.sub]` name for @p own (this decl's own Dbg::sym), registered for
    /// by-name lookups (e.g. `compile.named`); @p own == sym.tag (no enclosing `mod`) omits the `.sub` part.
    Sym qualified(Driver& driver, Sym own) const {
        auto base = sym.plugin.str() + "." + sym.tag.str();
        return driver.sym(own == sym.tag ? base : base + "." + own.str());
    }

    struct {
        Sym plugin, tag;
    } sym;

    struct {
        tag_t tag;
    } id;

    fe::Vector<fe::Vector<Sym>> subs; ///< List of subs which is a list of aliases.
    /// The constructor or field names its variant or sigma declares, by index; an unnamed field is the empty Sym.
    fe::Vector<Sym> names;
    bool fields = false; ///< Are AnnexInfo::names the fields of a sigma rather than the constructors of a variant?
    Dbg normalizer;
    std::optional<bool> pi;
    bool fresh = true;
};

/// Owns the arena all AST nodes live in as well as the AnnexInfo%s of all plugins.
class AST {
public:
    AST(const AST&) = delete;
    AST(World&);
    AST(AST&&);
    ~AST();

    /// @name Getters
    ///@{
    World& world() const { return *world_; }
    Driver& driver() const { return world().driver(); }
    Error& error() const { return driver().error(); }
    ///@}

    /// @name Sym
    ///@{
    Sym sym(const char* s) { return driver().sym(s); }
    Sym sym(std::string_view s) { return driver().sym(s); }
    Sym sym(const std::string& s) { return driver().sym(s); }
    Sym sym_return() { return sym("return"); } ///< `"return"`.
    Sym sym_error() { return sym("_error_"); } ///< `"_error_"`.
    ///@}

    /// The VLA ranges of a fe::VLA node come **last**, in the order its `VLA_Types` declares them.
    template<class T, class... Args>
    auto ptr(Args&&... args) {
        return arena_.ref<const T>(std::forward<Args>(args)...);
    }

    /// An Arena-allocated copy of @p range - for the few lists that do not sit behind a node.
    template<class R, class T = std::ranges::range_value_t<R>>
    fe::View<T> copy(const R& range) {
        auto span = arena_.copy(range);
        return fe::View<T>(span.data(), span.size());
    }

    /// A fresh Scope for a ModDecl; owned here, as an AST node never runs a destructor.
    Scope& scope() { return scopes_.emplace_back(); }

    /// @name Manage Files
    /// A file is parsed exactly once; AST::file hands out the File every Import of that file shares.
    ///@{
    /// The slot of @p src and whether it is fresh; a fresh slot is `nullptr` until the caller fills it in.
    /// A `nullptr` slot that is *not* fresh is a file still being parsed further up the stack.
    std::pair<Ptr<File>&, bool> file(const fe::Src* src);
    ///@}

    /// @name Manage Annex
    ///@{
    /// Registers @p dbg as an annex, deriving `plugin`/`tag`/`sub` from @p dbg's source file and @p s's current
    /// `mod`-nesting (Scopes::mod_depth/Scopes::enclosing_mod); @p sub_id receives @p dbg's `sub` index within its
    /// tag. `nullptr` if @p dbg is anonymous or nests more than one `mod` deep.
    /// @p type, if given, is the declared body whose constructor or field names become AnnexInfo::names.
    AnnexInfo* name2annex(Scopes& s, Dbg dbg, sub_t* sub_id, const Expr* type = nullptr);
    const auto& plugin2annexes(Sym plugin) { return plugin2sym2annex_[plugin]; }

    /// An `anx` alias in one plugin for an annex of another, e.g. `anx M = core.M;` in `mem`.
    struct ForeignAlias {
        Sym alias;
        const AnnexInfo* annex;
        sub_t sub;
    };
    void add_foreign_alias(Sym plugin, ForeignAlias alias) { plugin2foreign_aliases_[plugin].emplace_back(alias); }
    const auto& plugin2foreign_aliases(Sym plugin) { return plugin2foreign_aliases_[plugin]; }
    ///@}

    /// @name bootstrap
    ///@{
    void bootstrap(Sym plugin, std::ostream& h);
    void bootstrap_py(Sym plugin, std::ostream& h);
    ///@}

    friend void swap(AST& a1, AST& a2) noexcept {
        using std::swap;
        // clang-format off
        swap(a1.world_,  a2.world_ );
        swap(a1.arena_,  a2.arena_ );
        swap(a1.files_,  a2.files_ );
        swap(a1.scopes_, a2.scopes_);
        swap(a1.plugin2sym2annex_, a2.plugin2sym2annex_);
        swap(a1.plugin2foreign_aliases_, a2.plugin2foreign_aliases_);
        // clang-format on
    }

private:
    // Out of line: a container of Ptr<File> would instantiate Arena::Deleter for the still incomplete File.
    struct Files;

    World* world_ = nullptr;
    fe::Arena arena_;
    std::unique_ptr<Files> files_;
    std::deque<Scope> scopes_;
    // Inner map must be pointer-stable: name2annex() hands out `AnnexInfo*`s that are cached in AST nodes,
    // so the elements must not be relocated when further annexes are inserted into the same plugin.
    std::unordered_map<fe::Sym, std::unordered_map<fe::Sym, AnnexInfo>> plugin2sym2annex_;
    std::unordered_map<fe::Sym, std::vector<ForeignAlias>> plugin2foreign_aliases_;
};

/// Base class of all AST nodes.
class Node : public fe::RuntimeCast<Node> {
protected:
    Node(Loc loc)
        : loc_(loc) {
        // Loc::operator+ takes src from its left operand, so a hull across two files points outside its own Src.
        assert((!loc.begin == !loc.end && loc.begin <= loc.end) && "malformed Loc");
        assert((!loc || !loc.src || (loc.src->contains(loc.begin) && loc.src->contains(loc.end)))
               && "Loc outside its Src");
    }

public:
    Loc loc() const { return loc_; }

    virtual void stream(fe::Tab&, std::ostream&) const = 0;
    void dump() const;

private:
    Loc loc_;
};

/// Base class of all expressions.
class Expr : public Node {
protected:
    Expr(Loc loc)
        : Node(loc) {}

public:
    /// @name emit
    /// Each installs this Expr's Loc as World::get_loc, so every Def emitted underneath is blamed on it.
    ///@{
    const Def* emit(Emitter&) const;
    const Def* emit_decl(Emitter&, const Def* type) const;
    void emit_body(Emitter&, const Def* decl) const;
    ///@}

    virtual void bind(Scopes&) const = 0;

private:
    virtual const Def* emit_(Emitter&) const = 0;
    virtual const Def* emit_decl_(Emitter&, const Def* /*type*/) const { std::unreachable(); }
    virtual void emit_body_(Emitter&, const Def* /*decl*/) const { std::unreachable(); }
};

/// Base class of all declarations; caches the emitted Decl::def.
class Decl : public Node {
protected:
    Decl(Loc loc)
        : Node(loc) {}

public:
    const Def* def() const { return def_; }

    /// The name this Decl introduces; anonymous if it has none.
    virtual Dbg dbg() const { return Dbg(loc(), Sym()); }
    /// Non-`nullptr` if this Decl is a module whose members a Path may walk into.
    virtual const Scope* scope() const { return nullptr; }
    /// The `(AnnexInfo, sub)` slot this decl registered itself under as an annex; `{nullptr, 0}` if it isn't one.
    virtual std::pair<AnnexInfo*, sub_t> annex_sub() const { return {nullptr, 0}; }
    /// A ValDecl's own Vis; `Pub` for anything else (Ptrn, Import, ...), which the `priv`/`pub` system ignores.
    virtual Vis vis() const { return Vis::Pub; }
    /// Whether this decl crosses the Mim/native boundary (`extern`); body presence picks which side supplies it.
    virtual bool is_extern() const { return false; }
    /// Whether this decl is registered as a compiler-exposed annex (`anx`).
    virtual bool is_anx() const { return false; }

protected:
    mutable const Def* def_ = nullptr;
};

/// `dbg` bound to a Def whose type the binding construct dictates; hence, it carries no annotation.
class IdDecl : public Decl {
public:
    IdDecl(Dbg dbg)
        : Decl(dbg.loc())
        , dbg_(dbg) {}

    Dbg dbg() const override { return dbg_; }

    void bind(Scopes&, bool quiet = false) const;
    const Def* emit(const Def*) const;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
};

/// Base class of all declarations that bind values.
class ValDecl : public Decl {
protected:
    ValDecl(Loc loc, Mods mods = {})
        : Decl(loc)
        , mods_(mods) {}

public:
    Vis vis() const override { return mods_.resolved_vis(); }
    bool is_extern() const override { return mods_.is_extern; }
    bool is_anx() const override { return mods_.is_anx; }
    const Mods& mods() const { return mods_; }

    virtual void bind(Scopes&) const  = 0;
    virtual void emit(Emitter&) const = 0;

private:
    Mods mods_;
};

/*
 * Ptrn
 */

/// Base class of all patterns.
class Ptrn : public Decl {
public:
    Ptrn(Loc loc)
        : Decl(loc) {}

    virtual bool is_implicit() const { return false; }

    virtual void bind(Scopes&, bool rebind, bool quiet) const = 0;
    virtual const Def* emit_value(Emitter&, const Def*) const = 0;
    virtual const Def* emit_type(Emitter&) const              = 0;

    /// Ptrn::emit_value on @p def's @p i-th of @p n projections - with this Ptrn's Loc, so the
    /// projection is blamed on the binder it introduces instead of on the enclosing pattern.
    const Def* emit_proj(Emitter&, const Def* def, size_t n, size_t i) const;

    [[nodiscard]] static Ptr<Expr> to_expr(AST&, Ptr<Ptrn>);
};

/// Erroneous pattern.
class ErrorPtrn : public Ptrn {
public:
    ErrorPtrn(Loc loc)
        : Ptrn(loc) {}

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;
};

/// `dbg: type`
class IdPtrn : public Ptrn {
public:
    IdPtrn(Loc loc, Dbg dbg, Ptr<Expr> type)
        : Ptrn(loc)
        , dbg_(dbg)
        , type_(type) {}

    Dbg dbg() const override { return dbg_; }
    const Expr* type() const { return type_.get(); }
    std::pair<AnnexInfo*, sub_t> annex_sub() const override { return {annex_, sub_}; }
    /// Mirrors the enclosing LetDecl::vis/is_anx, since a `let`'s Ptrn - not the LetDecl - is what a Path resolves to.
    Vis vis() const override { return vis_; }
    bool is_anx() const override { return anx_; }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    Ptr<Expr> type_;
    mutable const Def* emitted_type_ = nullptr;
    mutable AnnexInfo* annex_        = nullptr;
    mutable sub_t sub_               = 0;
    mutable Vis vis_                 = Vis::Priv;
    mutable bool anx_                = false;

    friend class LetDecl;
    friend class GrpPtrn;
};

/// `dbg_i` of a group `dbg_0 ... dbg_n-1: type` for `i > 0`; the leading IdPtrn::id owns `type`.
class GrpPtrn : public Ptrn {
public:
    GrpPtrn(Dbg dbg, const IdPtrn* id)
        : Ptrn(dbg.loc())
        , dbg_(dbg)
        , id_(id) {}

    Dbg dbg() const override { return dbg_; }
    const IdPtrn* id() const { return id_; }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    const IdPtrn* id_;
};

/// `ptrn as dbg`
class AliasPtrn : public Ptrn {
public:
    AliasPtrn(Loc loc, Ptr<Ptrn> ptrn, Dbg dbg)
        : Ptrn(loc)
        , ptrn_(ptrn)
        , dbg_(dbg) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    Dbg dbg() const override { return dbg_; }
    bool is_implicit() const override { return ptrn()->is_implicit(); }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Ptr<Ptrn> ptrn_;
    Dbg dbg_;
};

/// `(ptrn_0, ..., ptrn_n-1)`, `[ptrn_0, ..., ptrn_n-1]`, or `{ptrn_0, ..., ptrn_n-1}`
class TuplePtrn : public Ptrn, public fe::VLA<TuplePtrn> {
public:
    using VLA_Types = std::tuple<Ptr<Ptrn>>;

    TuplePtrn(Loc loc, Tok::Tag delim_l)
        : Ptrn(loc)
        , delim_l_(delim_l) {}

    Tok::Tag delim_l() const { return delim_l_; }
    Tok::Tag delim_r() const { return Tok::delim_l2r(delim_l()); }
    bool is_brckt() const { return delim_l() == Tok::Tag::D_brckt_l; }
    bool is_implicit() const override { return delim_l_ == Tok::Tag::D_brace_l; }

    auto ptrns() const { return vla<0>(); }
    const Ptrn* ptrn(size_t i) const { return ptrns()[i].get(); }
    Sym field(size_t i) const; ///< The field name of ptrn(@p i), or the empty Sym if it binds none.
    size_t num_ptrns() const { return ptrns().size(); }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const;
    const Def* emit_body(Emitter&, const Def* decl) const;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Tok::Tag delim_l_;
};

/*
 * Expr
 */

/// Erroneous expression.
class ErrorExpr : public Expr {
public:
    ErrorExpr(Loc loc)
        : Expr(loc) {}

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
};

/// `?`
class HoleExpr : public Expr {
public:
    HoleExpr(Loc loc)
        : Expr(loc) {}

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
};

/// `dbg_0.....dbg_n-1`.
class Path : public Node, public fe::VLA<Path> {
public:
    using VLA_Types = std::tuple<Dbg>;

    Path(Loc loc)
        : Node(loc) {}

    auto dbgs() const { return vla<0>(); }
    Dbg front() const { return dbgs().front(); }
    Dbg back() const { return dbgs().back(); }
    const Decl* decl() const { return decl_; }

    void bind(Scopes&, bool quiet = false) const;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    mutable const Decl* decl_ = nullptr;
};

/// `path`
class PathExpr : public Expr {
public:
    PathExpr(Ptr<Path> path)
        : Expr(path->loc())
        , path_(path) {}

    const Path* path() const { return path_.get(); }
    Dbg dbg() const { return path_->back(); }
    const Decl* decl() const { return path_->decl(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Path> path_;
};

/// `tag`
class PrimaryExpr : public Expr {
public:
    PrimaryExpr(Loc loc, Tok::Tag tag)
        : Expr(loc)
        , tag_(tag) {}
    PrimaryExpr(Tok tok)
        : PrimaryExpr(tok.loc(), tok.tag()) {}

    Tok::Tag tag() const { return tag_; }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok::Tag tag_;
};

/// `tok:type`
class LitExpr : public Expr {
public:
    LitExpr(Loc loc, Tok tok, Ptr<Expr> type)
        : Expr(loc)
        , tok_(tok)
        , type_(type) {}

    Tok tok() const { return tok_; }
    Tok::Tag tag() const { return tok_.tag(); }
    const Expr* type() const { return type_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok tok_;
    Ptr<Expr> type_;
};

/// `decls expr` or `expr where decls` if DeclExpr::is_where.
class DeclExpr : public Expr, public fe::VLA<DeclExpr> {
public:
    using VLA_Types = std::tuple<Ptr<ValDecl>>;

    DeclExpr(Loc loc, Ptr<Expr> expr, bool is_where)
        : Expr(loc)
        , expr_(expr)
        , is_where_(is_where) {}

    auto decls() const { return vla<0>(); }
    bool is_where() const { return is_where_; }
    const Expr* expr() const { return expr_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> expr_;
    bool is_where_;
};

/// `Type level`
class TypeExpr : public Expr {
public:
    TypeExpr(Loc loc, Ptr<Expr> level)
        : Expr(loc)
        , level_(level) {}

    const Expr* level() const { return level_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> level_;
};

/// `Rule dom`
class RuleExpr : public Expr {
public:
    RuleExpr(Loc loc, Ptr<Expr> dom)
        : Expr(loc)
        , dom_(dom) {}

    const Expr* dom() const { return dom_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> dom_;
};

// prefix/infix

/// `op rhs`; PrefixExpr::op picks the meaning - see MIM_PREFIX.
class PrefixExpr : public Expr {
public:
    PrefixExpr(Loc loc, Tok op, Ptr<Expr> rhs)
        : Expr(loc)
        , op_(op)
        , rhs_(rhs) {}

    Tok op() const { return op_; }
    const Expr* rhs() const { return rhs_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok op_;
    Ptr<Expr> rhs_;
};

/// `lhs op rhs`; InfixExpr::op picks the meaning - see MIM_INFIX.
class InfixExpr : public Expr {
public:
    /// @p callee is the `` `op `` a MIM_INFIX_SUGAR operator desugars to and `nullptr` for MIM_INFIX_CORE.
    InfixExpr(Loc loc, Ptr<Expr> lhs, Tok op, Ptr<Expr> rhs, Ptr<Expr> callee)
        : Expr(loc)
        , lhs_(lhs)
        , op_(op)
        , rhs_(rhs)
        , callee_(callee) {}

    const Expr* lhs() const { return lhs_.get(); }
    Tok op() const { return op_; }
    const Expr* rhs() const { return rhs_.get(); }
    const Expr* callee() const { return callee_.get(); }

    /// @returns @p expr if it is an InfixExpr whose operator is @p tag.
    static const InfixExpr* isa_op(Tok::Tag tag, const Expr* expr) {
        auto infix = expr->isa<InfixExpr>();
        return infix && infix->op().isa(tag) ? infix : nullptr;
    }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    /// Resolves this `#`'s index against @p tup - a simple path may name a field of tup's Sigma.
    const Def* emit_index(Emitter&, const Def* tup) const;

    const Def* emit_(Emitter&) const override;
    const Def* emit_decl_(Emitter&, const Def* type) const override;
    void emit_body_(Emitter&, const Def* decl) const override;

    Ptr<Expr> lhs_;
    Tok op_;
    Ptr<Expr> rhs_;
    Ptr<Expr> callee_;
    mutable Pi* pi_ = nullptr;
};

/// `match scrutinee with | arm_0 | ... | arm_n-1`
class MatchExpr : public Expr, public fe::VLA<MatchExpr> {
public:
    /// `ptrn => body`, `ctor ptrn => body`, or `index [ptrn] => body` of a MatchExpr.
    class Arm : public Node {
    public:
        Arm(Loc loc, std::optional<nat_t> index, Dbg ctor, Ptr<Ptrn> ptrn, Ptr<Expr> body)
            : Node(loc)
            , index_(index)
            , ctor_(ctor)
            , ptrn_(ptrn)
            , body_(body) {}

        const Ptrn* ptrn() const { return ptrn_.get(); }
        const Expr* body() const { return body_.get(); }
        /// The case this arm selects by literal index, as in `1 (h, t)`.
        std::optional<nat_t> index() const { return index_; }
        /// The constructor in `Cons (h, t)`; a bare `Nil` is an unannotated IdPtrn instead.
        Dbg ctor() const { return ctor_; }

        virtual void bind(Scopes&) const;
        /// @p dom is the case's payload type if the scrutinee is a Variant; otherwise ptrn() determines the domain.
        Lam* emit(Emitter&, const Def* dom = nullptr) const;
        void stream(fe::Tab&, std::ostream&) const override;

    private:
        std::optional<nat_t> index_;
        Dbg ctor_;
        Ptr<Ptrn> ptrn_;
        Ptr<Expr> body_;
    };

    using VLA_Types = std::tuple<Ptr<Arm>>;

    MatchExpr(Loc loc, Ptr<Expr> scrutinee)
        : Expr(loc)
        , scrutinee_(scrutinee) {}

    const Expr* scrutinee() const { return scrutinee_.get(); }
    auto arms() const { return vla<0>(); }
    const Arm* arm(size_t i) const { return arms()[i].get(); }
    size_t num_arms() const { return arms().size(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
    const Def* emit_variant(Emitter&, const Def* scrutinee, const Variant*) const;

    Ptr<Expr> scrutinee_;
};

/// `| ctor_0: type_0 | ... | ctor_n-1: type_n-1`; a constructor without a type carries `[]`.
class VariantExpr : public Expr, public fe::VLA<VariantExpr> {
public:
    /// `dbg: type` of a VariantExpr.
    class Ctor : public Node {
    public:
        Ctor(Loc loc, Dbg dbg, Ptr<Expr> type)
            : Node(loc)
            , dbg_(dbg)
            , type_(type) {}

        Dbg dbg() const { return dbg_; }
        const Expr* type() const { return type_.get(); } ///< `nullptr` if omitted.

        void stream(fe::Tab&, std::ostream&) const override;

    private:
        Dbg dbg_;
        Ptr<Expr> type_;
    };

    using VLA_Types = std::tuple<Ptr<Ctor>>;

    VariantExpr(Loc loc)
        : Expr(loc) {}

    auto ctors() const { return vla<0>(); }
    const Ctor* ctor(size_t i) const { return ctors()[i].get(); }
    size_t num_ctors() const { return ctors().size(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
    const Def* emit_decl_(Emitter&, const Def* type) const override;
    void emit_body_(Emitter&, const Def* decl) const override;
    DefVec emit_payloads(Emitter&) const;
};

// lam

/// `dom → codom`, `Cn dom`, or `Fn dom → codom` depending on PiExpr::tag.
class PiExpr : public Expr {
public:
    /// One `dom` of a PiExpr: `ptrn` with an optional `-> ret` type.
    class Dom : public Node {
    public:
        Dom(Loc loc, Ptr<Ptrn> ptrn, Ptr<Expr> ret_type)
            : Node(loc)
            , ptrn_(ptrn)
            , ret_type_(ret_type) {}

        bool is_implicit() const { return ptrn_->is_implicit(); }
        const Ptrn* ptrn() const { return ptrn_.get(); }
        const Expr* ret_type() const { return ret_type_.get(); }

        virtual void bind(Scopes&, bool quiet = false) const;
        virtual void emit_type(Emitter&) const;
        void stream(fe::Tab&, std::ostream&) const override;

    protected:
        mutable Pi* decl_ = nullptr;
        mutable Pi* pi_   = nullptr;

    private:
        Ptr<Ptrn> ptrn_;
        Ptr<Expr> ret_type_;

        friend class PiExpr;
    };

    PiExpr(Loc loc, Tok::Tag tag, Ptr<Dom> dom, Ptr<Expr> codom)
        : Expr(loc)
        , tag_(tag)
        , dom_(dom)
        , codom_(codom) {}

private:
    Tok::Tag tag() const { return tag_; }
    const Dom* dom() const { return dom_.get(); }
    const Expr* codom() const { return codom_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
    const Def* emit_decl_(Emitter&, const Def* type) const override;
    void emit_body_(Emitter&, const Def* decl) const override;

    Tok::Tag tag_;
    Ptr<Dom> dom_;
    Ptr<Expr> codom_;
};

/// Wraps a LamDecl as Expr.
class LamExpr : public Expr {
public:
    LamExpr(Ptr<LamDecl> lam);

    const LamDecl* lam() const { return lam_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
    const Def* emit_decl_(Emitter&, const Def* type) const override;
    void emit_body_(Emitter&, const Def* decl) const override;

    Ptr<LamDecl> lam_;
};

/// `callee arg`
class AppExpr : public Expr {
public:
    AppExpr(Loc loc, Ptr<Expr> callee, Ptr<Expr> arg)
        : Expr(loc)
        , callee_(callee)
        , arg_(arg) {}

    const Expr* callee() const { return callee_.get(); }
    const Expr* arg() const { return arg_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> callee_;
    Ptr<Expr> arg_;
};

/// `ret ptrn = callee $ arg; body`
class RetExpr : public Expr {
public:
    RetExpr(Loc loc, Ptr<Ptrn> ptrn, Ptr<Expr> callee, Ptr<Expr> arg, Ptr<Expr> body)
        : Expr(loc)
        , ptrn_(ptrn)
        , callee_(callee)
        , arg_(arg)
        , body_(body) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    const Expr* callee() const { return callee_.get(); }
    const Expr* arg() const { return arg_.get(); }
    const Expr* body() const { return body_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Ptrn> ptrn_;
    Ptr<Expr> callee_;
    Ptr<Expr> arg_;
    Ptr<Expr> body_;
};
// tuple

/// Wraps a TuplePtrn as Expr.
class SigmaExpr : public Expr {
public:
    SigmaExpr(Ptr<TuplePtrn> ptrn)
        : Expr(ptrn->loc())
        , ptrn_(ptrn) {}

    const TuplePtrn* ptrn() const { return ptrn_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
    const Def* emit_decl_(Emitter&, const Def* type) const override;
    void emit_body_(Emitter&, const Def* decl) const override;

    Ptr<TuplePtrn> ptrn_;
};

/// `(elem_0, ..., elem_n-1)`
class TupleExpr : public Expr, public fe::VLA<TupleExpr> {
public:
    using VLA_Types = std::tuple<Ptr<Expr>>;

    TupleExpr(Loc loc)
        : Expr(loc) {}

    auto elems() const { return vla<0>(); }
    const Expr* elem(size_t i) const { return elems()[i].get(); }
    size_t num_elems() const { return elems().size(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
};

/// `«index: shape; body»` or `‹index: shape; body›` if SeqExpr::is_pack; `index: ` is optional.
class SeqExpr : public Expr {
public:
    SeqExpr(Loc loc, bool is_pack, Ptr<IdDecl> index, Ptr<Expr> shape, Ptr<Expr> body)
        : Expr(loc)
        , is_pack_(is_pack)
        , index_(index)
        , shape_(shape)
        , body_(body) {}

    bool is_pack() const { return is_pack_; }
    const IdDecl* index() const { return index_.get(); }
    const Expr* shape() const { return shape_.get(); }
    const Expr* body() const { return body_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    bool is_pack_;
    Ptr<IdDecl> index_;
    Ptr<Expr> shape_;
    Ptr<Expr> body_;
};

/// `«body»` or `‹body›` if SingleExpr::is_wrap.
class SingleExpr : public Expr {
public:
    SingleExpr(Loc loc, bool is_wrap, Ptr<Expr> body)
        : Expr(loc)
        , is_wrap_(is_wrap)
        , body_(body) {}

    bool is_wrap() const { return is_wrap_; }
    const Expr* body() const { return body_.get(); }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    bool is_wrap_;
    Ptr<Expr> body_;
};

/*
 * Decls
 */

/// `import "file"|name [as alias|*];`, `plugin name [as alias|*];`, or `use path [as alias|*];`
/// `as *` splices instead of naming; a `use` without an `as` is sugar for `as *`.
/// Makes another module available here: `import`/`plugin` pull in a File, `use` walks a Path.
/// Either that module is bound under UseDecl::dbg, or its public members are spliced into the current scope.
/// Defaults to `priv`; whatever it binds or splices carries this decl's own Vis, so only `pub` re-exports.
class UseDecl : public ValDecl {
public:
    /// `use path [as alias|*];`; no @p alias means `as *`.
    UseDecl(Loc loc, Mods mods, Ptr<Path> path, Dbg alias)
        : ValDecl(loc, mods)
        , tag_(Tok::Tag::K_use)
        , path_(path)
        , alias_(alias)
        , splice_(!alias) {}

    /// `import`/`plugin`; @p name is the module name, derived from @p file_path if there is one.
    /// The File itself is owned by the AST and shared by all its importers.
    UseDecl(Loc loc, Mods mods, Tok::Tag tag, Ptr<Path> path, Sym file_path, Dbg alias, bool splice, const File* file)
        : ValDecl(loc, mods)
        , tag_(tag)
        , path_(path)
        , alias_(alias)
        , file_path_(file_path)
        , file_(file)
        , splice_(splice) {}

    Tok::Tag tag() const { return tag_; } ///< `import`, `plugin`, or `use`.
    bool is_import() const { return tag_ != Tok::Tag::K_use; }
    const Path* path() const { return path_.get(); } ///< Module path of a `use`; module name of an `import`/`plugin`.
    Dbg alias() const { return alias_; }             ///< Empty, unless the source spelled `as I`.
    Sym file_path() const { return file_path_; }     ///< Spelling of an `import "..."`; empty otherwise.
    bool is_file_path() const { return (bool)file_path_; }
    const File* file() const { return file_; }
    bool is_splice() const { return splice_; } ///< `as *`: splice the module's public members, bind no name.

    /// The name this decl introduces; anonymous if it splices instead.
    Dbg dbg() const override { return is_splice() ? Dbg() : alias_ ? alias_ : path_->back(); }

    const Scope* scope() const override { return scope_; }
    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    /// The module this decl refers to; `nullptr` if it doesn't resolve - the error has already been emitted.
    const Scope* module(Scopes&) const;

    Tok::Tag tag_;
    Ptr<Path> path_;
    Dbg alias_;
    Sym file_path_;
    const File* file_ = nullptr;
    bool splice_;
    mutable const Scope* scope_ = nullptr;
};

/// `let ptrn = value;`
class LetDecl : public ValDecl {
public:
    LetDecl(Loc loc, Mods mods, Ptr<Ptrn> ptrn, Ptr<Expr> value)
        : ValDecl(loc, mods)
        , ptrn_(ptrn)
        , value_(value) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    const Expr* value() const { return value_.get(); }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Ptr<Ptrn> ptrn_;
    Ptr<Expr> value_;
};

/// `axm dbg: type, normalizer, curry, trip;` or a family `axm (name, ...): type, normalizer, curry, trip;`.
/// A family prefixed with `tag.` binds as a module and nests its names as subtags of the annex `tag`.
class AxmDecl : public ValDecl, public fe::VLA<AxmDecl> {
public:
    /// `dbg = alias_0 = ... = alias_n-1` - one axiom of the family, which each of its names binds to.
    class Name : public Decl, public fe::VLA<Name> {
    public:
        using VLA_Types = std::tuple<Dbg>;

        Name(Loc loc)
            : Decl(loc) {}

        auto names() const { return vla<0>(); }
        Dbg dbg() const override { return names().front(); }
        auto aliases() const { return names().subspan(1); }
        bool is_anx() const override { return true; }
        std::pair<AnnexInfo*, sub_t> annex_sub() const override { return {annex_, sub_}; }

        void stream(fe::Tab&, std::ostream&) const override;

    private:
        mutable AnnexInfo* annex_ = nullptr;
        mutable sub_t sub_        = 0;

        friend class AxmDecl;
    };

    using VLA_Types = std::tuple<Ptr<Name>>;

    AxmDecl(Loc l, Vis v, Dbg mod, Scope* members, bool is_family, Ptr<Expr> type, Dbg normalizer, Tok curry, Tok trip)
        : ValDecl(l, Mods{v, /*is_extern=*/false, /*is_anx=*/true})
        , mod_(mod)
        , members_(members)
        , is_family_(is_family)
        , type_(type)
        , normalizer_(normalizer)
        , curry_(curry)
        , trip_(trip) {}

    /// The `mod.` prefix, if any.
    Dbg dbg() const override { return mod_; }
    const Scope* scope() const override { return members_; }
    auto names() const { return vla<0>(); }
    /// Whether the names are spelled as a parenthesized list, even if it is just one.
    bool is_family() const { return is_family_; }
    const Expr* type() const { return type_.get(); }
    Dbg normalizer() const { return normalizer_; }
    Tok curry() const { return curry_; }
    Tok trip() const { return trip_; }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    Dbg mod_;
    Scope* members_;
    bool is_family_;
    Ptr<Expr> type_;
    Dbg normalizer_;
    Tok curry_, trip_;
};

/// `rec dbg = body;`
class RecDecl : public ValDecl {
public:
    RecDecl(Loc loc, Mods mods, Dbg dbg, Ptr<Expr> body)
        : ValDecl(loc, mods)
        , dbg_(dbg)
        , body_(body) {}

    Dbg dbg() const override { return dbg_; }
    const Expr* body() const { return body_.get(); }
    /// Only such a body is emitted as a mutable, so only it may refer to the declaration itself.
    bool has_mut_body() const;

    void bind(Scopes&) const override;
    virtual void bind_decl(Scopes&) const;
    virtual void bind_body(Scopes&) const;

    void emit(Emitter&) const override;
    virtual void emit_decl(Emitter&) const;
    virtual void emit_body(Emitter&) const;

    void stream(fe::Tab&, std::ostream&) const override;
    /// Streams this declaration without modifiers and without the trailing `;`.
    virtual void stream_(fe::Tab&, std::ostream&) const;
    std::pair<AnnexInfo*, sub_t> annex_sub() const override { return {annex_, sub_}; }

protected:
    mutable AnnexInfo* annex_ = nullptr;
    mutable sub_t sub_        = 0;

private:
    Dbg dbg_;
    Ptr<Expr> body_;
};

/// `nom dbg = body;` - a nominal newtype.
class NomDecl : public RecDecl {
public:
    NomDecl(Loc loc, Mods mods, Dbg dbg, Ptr<Expr> body)
        : RecDecl(loc, mods, dbg, body) {}

    /// The visibility of this `nom`'s representation: that of the enclosing top-level declaration.
    Vis rep_vis() const { return rep_vis_; }

    void bind_body(Scopes&) const override;
    void emit_decl(Emitter&) const override;
    void emit_body(Emitter&) const override;
    /// Emits a `nom` without a mutable body as soon as a sibling refers to it; `nullptr` on a cycle.
    const Def* emit_ahead(Emitter&) const;
    /// Sets @p lam's body to this LamDecl::nom_sugar before emitting the latter's body.
    void emit_sugar(Emitter&, Lam* lam) const;
    void stream_(fe::Tab&, std::ostream&) const override;

private:
    void emit_nom(Emitter&, const Def* body) const;

    mutable bool emitting_ = false;
    mutable Vis rep_vis_   = Vis::Pub;
};

/// `tag dbg dom_0 ... dom_n-1: codom = body;` with LamDecl::tag `lam`/`con`/`fun` or anonymous `λ`/`cn`/`fn`.
class LamDecl : public RecDecl, public fe::VLA<LamDecl> {
public:
    /// One `dom` of a LamDecl: `ptrn@(filter)` with an optional `: ret` type.
    class Dom : public PiExpr::Dom {
    public:
        Dom(Loc loc, Ptr<Ptrn> ptrn, Ptr<Expr> filter, Ptr<Expr> ret_type, Ptr<IdDecl> ret)
            : PiExpr::Dom(loc, ptrn, ret_type)
            , filter_(filter)
            , ret_(ret) {}

        const Expr* filter() const { return filter_.get(); }
        const IdDecl* ret() const { return ret_.get(); }

        void bind(Scopes&, bool quiet = false) const override;
        Lam* emit_value(Emitter&) const;
        void stream(fe::Tab&, std::ostream&) const override;

    private:
        Ptr<Expr> filter_;
        Ptr<IdDecl> ret_;
        mutable Lam* lam_;

        friend class LamDecl;
    };

    using VLA_Types = std::tuple<Ptr<Dom>>;

    LamDecl(Loc loc, Mods mods, Tok::Tag tag, Dbg dbg, Ptr<Expr> codom, Ptr<Expr> body, const NomDecl* nom_sugar)
        : RecDecl(loc, mods, dbg, body)
        , tag_(tag)
        , codom_(codom)
        , nom_sugar_(nom_sugar) {}

    Tok::Tag tag() const { return tag_; }
    auto doms() const { return vla<0>(); }
    const Dom* dom(size_t i) const { return doms()[i].get(); }
    size_t num_doms() const { return doms().size(); }
    const Expr* codom() const { return codom_.get(); }
    /// The `nom` of `lam I dom+ = nom I = e; I` if `nom I dom+ = e` desugared to this; within `e`, `I` is this LamDecl.
    const NomDecl* nom_sugar() const { return nom_sugar_; }

    void bind_decl(Scopes&) const override;
    void bind_body(Scopes&) const override;
    void emit_decl(Emitter&) const override;
    void emit_body(Emitter&) const override;
    void stream_(fe::Tab&, std::ostream&) const override;

private:
    Tok::Tag tag_;
    Ptr<Expr> codom_;
    const NomDecl* nom_sugar_;
};

/// `mutual decl* end` groups RecDecl%s that may refer to each other.
class MutualDecl : public ValDecl, public fe::VLA<MutualDecl> {
public:
    using VLA_Types = std::tuple<Ptr<RecDecl>>;

    MutualDecl(Loc loc)
        : ValDecl(loc) {}

    auto decls() const { return vla<0>(); }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;
};

/// `anx dbg = path;` - a compiler-exposed alias sharing its target's annex slot.
class AliasDecl : public ValDecl {
public:
    AliasDecl(Loc loc, Vis vis, Dbg dbg, Ptr<Path> path)
        : ValDecl(loc, Mods{vis, /*is_extern=*/false, /*is_anx=*/true})
        , dbg_(dbg)
        , path_(path) {}

    Dbg dbg() const override { return dbg_; }
    const Path* path() const { return path_.get(); }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;
    std::pair<AnnexInfo*, sub_t> annex_sub() const override { return {annex_, sub_}; }

private:
    Dbg dbg_;
    Ptr<Path> path_;
    mutable AnnexInfo* annex_ = nullptr;
    mutable sub_t sub_        = 0;
    mutable Sym foreign_; ///< Qualified name of a ForeignAlias, which is registered in its own plugin.
};

/// `rule dbg var: lhs when guard => rhs;` or `norm` instead of `rule` if RuleDecl::is_normalizer.
class RuleDecl : public ValDecl {
public:
    RuleDecl(Loc loc, Mods mods, Dbg dbg, Ptr<Ptrn> var, Ptr<Expr> lhs, Ptr<Expr> rhs, Ptr<Expr> guard, bool is_norm)
        : ValDecl(loc, mods)
        , dbg_(dbg)
        , var_(var)
        , lhs_(lhs)
        , rhs_(rhs)
        , guard_(guard)
        , is_norm_(is_norm) {}

    Dbg dbg() const override { return dbg_; }
    const Ptrn* var() const { return var_.get(); }
    const Expr* lhs() const { return lhs_.get(); }
    const Expr* rhs() const { return rhs_.get(); }
    const Expr* guard() const { return guard_.get(); }
    bool is_norm() const { return is_norm_; }

    void bind(Scopes&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    void emit(Emitter&) const override;

    Dbg dbg_;
    Ptr<Ptrn> var_;
    Ptr<Expr> lhs_;
    Ptr<Expr> rhs_;
    Ptr<Expr> guard_;
    bool is_norm_;
};

/// `mod dbg decls end`; also the base of the anonymous File.
class ModDecl : public ValDecl {
public:
    // A ModDecl is pure AST grouping - it never represents a single value, so it's never `extern`/`anx`.
    ModDecl(Loc loc, Vis vis, Dbg dbg, Scope& members, fe::View<Ptr<ValDecl>> decls)
        : ValDecl(loc, Mods{vis})
        , dbg_(dbg)
        , decls_(decls)
        , members_(&members) {}

    Dbg dbg() const override { return dbg_; }
    auto decls() const { return decls_; }

    const Scope* scope() const override { return members_; }
    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

protected:
    Scope& members() const { return *members_; }
    void bind_decls(Scopes&) const;
    void emit_decls(Emitter&) const;

private:
    Dbg dbg_;
    fe::View<Ptr<ValDecl>> decls_;
    Scope* members_;
};

/*
 * File
 */

/// The AST of one source file: an anonymous ModDecl that a UseDecl binds under a name of its own.
/// Unlike a nested ModDecl, its Scope is a barrier: a file must not see whoever imports it.
class File : public ModDecl {
public:
    File(Loc loc, Scope& members, fe::View<Ptr<ValDecl>> decls)
        : ModDecl(loc, Vis::Priv, Dbg(loc), members, decls) {}

    /// Imports the driver was told about via `-p`; they precede everything the file itself declares.
    auto implicit_imports() const { return implicit_imports_; }

    void add_implicit_imports(fe::View<Ptr<UseDecl>> imports) const { implicit_imports_ = imports; }

    void compile(AST&) const;
    void bind(AST&) const;
    void bind(Scopes&) const override;
    void emit(AST&) const;
    void emit(Emitter&) const override;
    void stream(fe::Tab&, std::ostream&) const override;

private:
    mutable fe::View<Ptr<UseDecl>> implicit_imports_;
    // A file is parsed, bound, and emitted exactly once, no matter how many UseDecls alias it.
    mutable bool bound_ = false, emitted_ = false;
};

AST load_plugins(World&, fe::View<std::string>);
inline AST load_plugin(World& w, std::string_view plugin) {
    return load_plugins(w, fe::View<std::string>({std::string(plugin)}));
}

} // namespace mim::ast
