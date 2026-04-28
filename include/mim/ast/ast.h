#pragma once

#include <deque>
#include <memory>

#include <fe/arena.h>
#include <fe/assert.h>
#include <fe/cast.h>

#include "mim/driver.h"

#include "mim/ast/tok.h"

namespace mim::ast {

class LamDecl;
class Module;
class Scopes;
class Emitter;

template<class T>
using Ptr = fe::Arena::Ptr<const T>;
template<class T>
using Ptrs                   = std::deque<Ptr<T>>;
/*             */ using Dbgs = std::deque<Dbg>;

struct AnnexInfo {
    AnnexInfo(Sym sym_plugin, Sym sym_tag, plugin_t id_plugin, tag_t id_tag)
        : sym{sym_plugin, sym_tag}
        , id{id_plugin, id_tag, 0, 0} {
        assert(Annex::mangle(sym_plugin) == id_plugin);
    }

    struct {
        Sym plugin, tag;
    } sym;
    struct {
        plugin_t plugin;
        tag_t tag;
        uint8_t curry, trip;
    } id;
    std::deque<std::deque<Sym>> subs; ///< List of subs which is a list of aliases.
    Dbg normalizer;
    std::optional<bool> pi;
    bool fresh = true;
};

class AST {
public:
    AST() = default;
    AST(World& world)
        : world_(&world) {}
    AST(AST&& other)
        : AST() {
        swap(*this, other);
    }
    ~AST();

    /// @name Getters
    ///@{
    World& world() { return *world_; }
    Driver& driver() { return world().driver(); }
    Error& error() { return err_; }
    const Error& error() const { return err_; }
    ///@}

    /// @name Sym
    ///@{
    Sym sym(const char* s) { return driver().sym(s); }
    Sym sym(std::string_view s) { return driver().sym(s); }
    Sym sym(const std::string& s) { return driver().sym(s); }
    Sym sym_anon() { return sym("_"); }        ///< `"_"`.
    Sym sym_return() { return sym("return"); } ///< `"return"`.
    Sym sym_error() { return sym("_error_"); } ///< `"_error_"`.
    ///@}

    template<class T, class... Args>
    auto ptr(Args&&... args) {
        return arena_.mk<const T>(std::forward<Args>(args)...);
    }

    /// @name Formatted Output
    ///@{
    // clang-format off
    template<class... Args> Error& error(Loc loc, const char* fmt, Args&&... args) const { return err_.error(loc, fmt, std::forward<Args>(args)...); }
    template<class... Args> Error& warn (Loc loc, const char* fmt, Args&&... args) const { return err_.warn (loc, fmt, std::forward<Args>(args)...); }
    template<class... Args> Error& note (Loc loc, const char* fmt, Args&&... args) const { return err_.note (loc, fmt, std::forward<Args>(args)...); }
    // clang-format on
    ///@}

    /// @name Manage Annex
    ///@{
    AnnexInfo* name2annex(Dbg dbg, sub_t*);
    const auto& plugin2annexes(Sym plugin) { return plugin2sym2annex_[plugin]; }
    ///@}

    void bootstrap(Sym plugin, std::ostream& h);

    friend void swap(AST& a1, AST& a2) noexcept {
        using std::swap;
        // clang-format off
        swap(a1.world_, a2.world_);
        swap(a1.arena_, a2.arena_);
        swap(a1.err_,   a2.err_);
        // clang-format on
    }

private:
    World* world_ = nullptr;
    fe::Arena arena_;
    mutable Error err_;
    absl::node_hash_map<fe::Sym, absl::node_hash_map<fe::Sym, AnnexInfo>> plugin2sym2annex_;
};

class Node : public fe::RuntimeCast<Node> {
protected:
    Node(Loc loc)
        : loc_(loc) {}
    virtual ~Node() {}

public:
    Loc loc() const { return loc_; }

    virtual std::ostream& stream(Tab&, std::ostream&) const = 0;
    void dump() const;

private:
    Loc loc_;
};

class Expr : public Node {
protected:
    Expr(Loc loc)
        : Node(loc) {}

public:
    using Prec = ast::Prec; ///< Backward-compatible alias; prefer the free-standing ast::Prec.

    const Def* emit(Emitter&) const;
    virtual void bind(Scopes&) const = 0;
    virtual const Def* emit_decl(Emitter&, const Def* /*type*/) const { fe::unreachable(); }
    virtual void emit_body(Emitter&, const Def* /*decl*/) const { fe::unreachable(); }

private:
    virtual const Def* emit_(Emitter&) const = 0;
};

class Decl : public Node {
protected:
    Decl(Loc loc)
        : Node(loc) {}

public:
    const Def* def() const { return def_; }

protected:
    mutable const Def* def_ = nullptr;
};

class ValDecl : public Decl {
protected:
    ValDecl(Loc loc)
        : Decl(loc) {}

public:
    virtual void bind(Scopes&) const  = 0;
    virtual void emit(Emitter&) const = 0;
};

/*
 * Ptrn
 */

class Ptrn : public Decl {
public:
    Ptrn(Loc loc)
        : Decl(loc) {}

    virtual bool is_implicit() const { return false; }

    virtual void bind(Scopes&, bool rebind, bool quiet) const = 0;
    virtual const Def* emit_value(Emitter&, const Def*) const = 0;
    virtual const Def* emit_type(Emitter&) const              = 0;

    [[nodiscard]] static Ptr<Expr> to_expr(AST&, Ptr<Ptrn>&&);
    [[nodiscard]] static Ptr<Ptrn> to_ptrn(Ptr<Expr>&&);
};

class ErrorPtrn : public Ptrn {
public:
    ErrorPtrn(Loc loc)
        : Ptrn(loc) {}

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;
};

/// `dbg: type`
class IdPtrn : public Ptrn {
public:
    IdPtrn(Loc loc, Dbg dbg, Ptr<Expr>&& type)
        : Ptrn(loc)
        , dbg_(dbg)
        , type_(std::move(type)) {}

    Dbg dbg() const { return dbg_; }
    const Expr* type() const { return type_.get(); }

    static Ptr<IdPtrn> make_type(AST& ast, Ptr<Expr>&& type) {
        auto loc = type->loc();
        return ast.ptr<IdPtrn>(loc, Dbg(loc, ast.sym_anon()), std::move(type));
    }
    static Ptr<IdPtrn> make_id(AST& ast, Dbg dbg, Ptr<Expr>&& type) {
        auto loc = (type && dbg) ? dbg.loc() + type->loc() : type ? type->loc() : dbg.loc();
        return ast.ptr<IdPtrn>(loc, dbg, std::move(type));
    }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    Ptr<Expr> type_;
};

/// If you have `x1 x2 x3 x4: T` it consists of 3 GrpPtrn%s and 1 IdPtrn while each GrpPtrn references the last IdPtrn.
class GrpPtrn : public Ptrn {
public:
    GrpPtrn(Dbg dbg, const IdPtrn* id)
        : Ptrn(dbg.loc())
        , dbg_(dbg)
        , id_(id) {}

    Dbg dbg() const { return dbg_; }
    const IdPtrn* id() const { return id_; }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    const IdPtrn* id_;
};

/// `ptrn as id`
class AliasPtrn : public Ptrn {
public:
    AliasPtrn(Loc loc, Ptr<Ptrn>&& ptrn, Dbg dbg)
        : Ptrn(loc)
        , ptrn_(std::move(ptrn))
        , dbg_(dbg) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    Dbg dbg() const { return dbg_; }
    bool is_implicit() const override { return ptrn()->is_implicit(); }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Ptr<Ptrn> ptrn_;
    Dbg dbg_;
};

/// `(ptrn_0, ..., ptrn_n-1)`, `[ptrn_0, ..., ptrn_n-1]`, or `{ptrn_0, ..., ptrn_n-1}`
class TuplePtrn : public Ptrn {
public:
    TuplePtrn(Loc loc, Tok::Tag delim_l, Ptrs<Ptrn>&& ptrns)
        : Ptrn(loc)
        , delim_l_(delim_l)
        , ptrns_(std::move(ptrns)) {}

    Tok::Tag delim_l() const { return delim_l_; }
    Tok::Tag delim_r() const { return Tok::delim_l2r(delim_l()); }
    bool is_paren() const { return delim_l() == Tok::Tag::D_paren_l; }
    bool is_brckt() const { return delim_l() == Tok::Tag::D_brckt_l; }
    bool is_implicit() const override { return delim_l_ == Tok::Tag::D_brace_l; }

    const auto& ptrns() const { return ptrns_; }
    const Ptrn* ptrn(size_t i) const { return ptrns_[i].get(); }
    size_t num_ptrns() const { return ptrns().size(); }

    void bind(Scopes&, bool rebind, bool quiet) const override;
    const Def* emit_value(Emitter&, const Def*) const override;
    const Def* emit_type(Emitter&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const;
    const Def* emit_body(Emitter&, const Def* decl) const;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Tok::Tag delim_l_;
    Ptrs<Ptrn> ptrns_;
};

/*
 * Expr
 */

class ErrorExpr : public Expr {
public:
    ErrorExpr(Loc loc)
        : Expr(loc) {}

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
};

class HoleExpr : public Expr {
public:
    HoleExpr(Loc loc)
        : Expr(loc) {}

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;
};

/// `sym`
class IdExpr : public Expr {
public:
    IdExpr(Dbg dbg)
        : Expr(dbg.loc())
        , dbg_(dbg) {}

    Dbg dbg() const { return dbg_; }
    const Decl* decl() const { return decl_; }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Dbg dbg_;
    mutable const Decl* decl_ = nullptr;
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
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok::Tag tag_;
};

/// `tok:type`
class LitExpr : public Expr {
public:
    LitExpr(Loc loc, Tok tok, Ptr<Expr>&& type)
        : Expr(loc)
        , tok_(tok)
        , type_(std::move(type)) {}

    Tok tok() const { return tok_; }
    Tok::Tag tag() const { return tok_.tag(); }
    const Expr* type() const { return type_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok tok_;
    Ptr<Expr> type_;
};

/// `decls e` or `e where decls` if @p where is `true`.
class DeclExpr : public Expr {
public:
    DeclExpr(Loc loc, Ptrs<ValDecl>&& decls, Ptr<Expr>&& expr, bool is_where)
        : Expr(loc)
        , decls_(std::move(decls))
        , expr_(std::move(expr))
        , is_where_(is_where) {}

    const auto& decls() const { return decls_; }
    bool is_where() const { return is_where_; }
    const Expr* expr() const { return expr_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptrs<ValDecl> decls_;
    Ptr<Expr> expr_;
    bool is_where_;
};

/// `Type level`
class TypeExpr : public Expr {
public:
    TypeExpr(Loc loc, Ptr<Expr>&& level)
        : Expr(loc)
        , level_(std::move(level)) {}

    const Expr* level() const { return level_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> level_;
};

/// Reform (type of a rule) `Rule type`.
class RuleExpr : public Expr {
public:
    RuleExpr(Loc loc, Ptr<Expr>&& meta_type)
        : Expr(loc)
        , meta_type_(std::move(meta_type)) {}

    const Expr* meta_type() const { return meta_type_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> meta_type_;
};

// union

/// `t1 ∪ t2`
class UnionExpr : public Expr {
public:
    UnionExpr(Loc loc, Ptrs<Expr>&& types)
        : Expr(loc)
        , types_(std::move(types)) {}

    const auto& types() const { return types_; }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptrs<Expr> types_;
};

// injection

/// `value inj t1 ∪ t2`
class InjExpr : public Expr {
public:
    InjExpr(Loc loc, Ptr<Expr>&& value, Ptr<Expr>&& type)
        : Expr(loc)
        , value_(std::move(value))
        , type_(std::move(type)) {}

    const Expr* value() const { return value_.get(); }
    const Expr* type() const { return type_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> value_;
    Ptr<Expr> type_;
};

// matching for destruction of sum types

// n-ary match
class MatchExpr : public Expr {
public:
    class Arm : public Node {
    public:
        Arm(Loc loc, Ptr<Ptrn>&& ptrn, Ptr<Expr>&& body)
            : Node(loc)
            , ptrn_(std::move(ptrn))
            , body_(std::move(body)) {}

        const Ptrn* ptrn() const { return ptrn_.get(); }
        const Expr* body() const { return body_.get(); }

        virtual void bind(Scopes&) const;
        Lam* emit(Emitter&) const;
        std::ostream& stream(Tab&, std::ostream&) const override;

    private:
        Ptr<Ptrn> ptrn_;
        Ptr<Expr> body_;
    };

    MatchExpr(Loc loc, Ptr<Expr>&& scrutinee, Ptrs<Arm>&& arms)
        : Expr(loc)
        , scrutinee_(std::move(scrutinee))
        , arms_(std::move(arms)) {}

    const Expr* scrutinee() const { return scrutinee_.get(); }
    const auto& arms() const { return arms_; }
    const Arm* arm(size_t i) const { return arms_[i].get(); }
    size_t num_arms() const { return arms_.size(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> scrutinee_;
    Ptrs<Arm> arms_;
};

// lam

/// `dom -> codom`
class ArrowExpr : public Expr {
public:
    ArrowExpr(Loc loc, Ptr<Expr>&& dom, Ptr<Expr>&& codom)
        : Expr(loc)
        , dom_(std::move(dom))
        , codom_(std::move(codom)) {}

private:
    const Expr* dom() const { return dom_.get(); }
    const Expr* codom() const { return codom_.get(); }

    void bind(Scopes&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const override;
    void emit_body(Emitter&, const Def* decl) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> dom_;
    Ptr<Expr> codom_;
    mutable Pi* decl_ = nullptr;
};

/// One of:
/// * `  {ptrn} → codom`
/// * `Cn prn`
/// * `Fn prn → codom`
class PiExpr : public Expr {
public:
    class Dom : public Node {
    public:
        Dom(Loc loc, Ptr<Ptrn>&& ptrn)
            : Node(loc)
            , ptrn_(std::move(ptrn)) {}

        bool is_implicit() const { return ptrn_->is_implicit(); }
        const Ptrn* ptrn() const { return ptrn_.get(); }
        const IdPtrn* ret() const { return ret_.get(); }

        void add_ret(AST& ast, Ptr<Expr>&& type) const {
            auto loc = type->loc();
            ret_     = ast.ptr<IdPtrn>(loc, Dbg(loc, ast.sym_return()), std::move(type));
        }

        virtual void bind(Scopes&, bool quiet = false) const;
        virtual void emit_type(Emitter&) const;
        std::ostream& stream(Tab&, std::ostream&) const override;

    protected:
        mutable Pi* decl_ = nullptr;
        mutable Pi* pi_   = nullptr;

    private:
        Ptr<Ptrn> ptrn_;
        mutable Ptr<IdPtrn> ret_;

        friend class PiExpr;
    };

    PiExpr(Loc loc, Tok::Tag tag, Ptr<Dom>&& dom, Ptr<Expr>&& codom)
        : Expr(loc)
        , tag_(tag)
        , dom_(std::move(dom))
        , codom_(std::move(codom)) {}

private:
    Tok::Tag tag() const { return tag_; }
    const Dom* dom() const { return dom_.get(); }
    const Expr* codom() const { return codom_.get(); }

    void bind(Scopes&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const override;
    void emit_body(Emitter&, const Def* decl) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Tok::Tag tag_;
    Ptr<Dom> dom_;
    Ptr<Expr> codom_;
};

/// Wraps a LamDecl as Expr.
class LamExpr : public Expr {
public:
    LamExpr(Ptr<LamDecl>&& lam);

    const LamDecl* lam() const { return lam_.get(); }

    void bind(Scopes&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const override;
    void emit_body(Emitter&, const Def* decl) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<LamDecl> lam_;
};

/// `callee arg`
class AppExpr : public Expr {
public:
    AppExpr(Loc loc, bool is_explicit, Ptr<Expr>&& callee, Ptr<Expr>&& arg)
        : Expr(loc)
        , is_explicit_(is_explicit)
        , callee_(std::move(callee))
        , arg_(std::move(arg)) {}

    bool is_explicit() const { return is_explicit_; }
    const Expr* callee() const { return callee_.get(); }
    const Expr* arg() const { return arg_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    bool is_explicit_;
    Ptr<Expr> callee_;
    Ptr<Expr> arg_;
};

/// `ret ptrn = callee $ arg; body`
class RetExpr : public Expr {
public:
    RetExpr(Loc loc, Ptr<Ptrn>&& ptrn, Ptr<Expr>&& callee, Ptr<Expr>&& arg, Ptr<Expr> body)
        : Expr(loc)
        , ptrn_(std::move(ptrn))
        , callee_(std::move(callee))
        , arg_(std::move(arg))
        , body_(std::move(body)) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    const Expr* callee() const { return callee_.get(); }
    const Expr* arg() const { return arg_.get(); }
    const Expr* body() const { return body_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Ptrn> ptrn_;
    Ptr<Expr> callee_;
    Ptr<Expr> arg_;
    Ptr<Expr> body_;
};
// tuple

/// Just wraps TuplePtrn as Expr.
class SigmaExpr : public Expr {
public:
    SigmaExpr(Ptr<TuplePtrn>&& ptrn)
        : Expr(ptrn->loc())
        , ptrn_(std::move(ptrn)) {}

    const TuplePtrn* ptrn() const { return ptrn_.get(); }

    void bind(Scopes&) const override;
    const Def* emit_decl(Emitter&, const Def* type) const override;
    void emit_body(Emitter&, const Def* decl) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<TuplePtrn> ptrn_;

    friend Ptr<Ptrn> Ptrn::to_ptrn(Ptr<Expr>&&);
};

/// `(elem_0, ..., elem_n-1)`
class TupleExpr : public Expr {
public:
    TupleExpr(Loc loc, Ptrs<Expr>&& elems)
        : Expr(loc)
        , elems_(std::move(elems)) {}

    const auto& elems() const { return elems_; }
    const Expr* elem(size_t i) const { return elems_[i].get(); }
    size_t num_elems() const { return elems().size(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptrs<Expr> elems_;
};

/// `«dbg: arity; body»` or `‹dbg: arity; body›`
class SeqExpr : public Expr {
public:
    SeqExpr(Loc loc, bool is_pack, Ptr<IdPtrn>&& arity, Ptr<Expr>&& body)
        : Expr(loc)
        , is_pack_(is_pack)
        , arity_(std::move(arity))
        , body_(std::move(body)) {}

    bool is_pack() const { return is_pack_; }
    const IdPtrn* arity() const { return arity_.get(); }
    const Expr* body() const { return body_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    bool is_pack_;
    Ptr<IdPtrn> arity_;
    Ptr<Expr> body_;
};

/// `tuple#index`
class ExtractExpr : public Expr {
public:
    ExtractExpr(Loc loc, Ptr<Expr>&& tuple, Ptr<Expr>&& index)
        : Expr(loc)
        , tuple_(std::move(tuple))
        , index_(std::move(index)) {}
    ExtractExpr(Loc loc, Ptr<Expr>&& tuple, Dbg index)
        : Expr(loc)
        , tuple_(std::move(tuple))
        , index_(index) {}

    const Expr* tuple() const { return tuple_.get(); }
    const auto& index() const { return index_; }
    const Decl* decl() const { return decl_; }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> tuple_;
    std::variant<Ptr<Expr>, Dbg> index_;
    mutable const Decl* decl_ = nullptr;
};

/// `ins(tuple, index, value)`
class InsertExpr : public Expr {
public:
    InsertExpr(Loc loc, Ptr<Expr>&& tuple, Ptr<Expr>&& index, Ptr<Expr>&& value)
        : Expr(loc)
        , tuple_(std::move(tuple))
        , index_(std::move(index))
        , value_(std::move(value)) {}

    const Expr* tuple() const { return tuple_.get(); }
    const Expr* index() const { return index_.get(); }
    const Expr* value() const { return value_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> tuple_;
    Ptr<Expr> index_;
    Ptr<Expr> value_;
};

/// `⦃ expr ⦄`
class UniqExpr : public Expr {
public:
    UniqExpr(Loc loc, Ptr<Expr>&& expr)
        : Expr(loc)
        , inhabitant_(std::move(expr)) {}

    const Expr* inhabitant() const { return inhabitant_.get(); }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    const Def* emit_(Emitter&) const override;

    Ptr<Expr> inhabitant_;
};

/*
 * Decls
 */

/// `let ptrn: type = value;`
class LetDecl : public ValDecl {
public:
    LetDecl(Loc loc, Ptr<Ptrn>&& ptrn, Ptr<Expr>&& value)
        : ValDecl(loc)
        , ptrn_(std::move(ptrn))
        , value_(std::move(value)) {}

    const Ptrn* ptrn() const { return ptrn_.get(); }
    const Expr* value() const { return value_.get(); }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Ptr<Ptrn> ptrn_;
    Ptr<Expr> value_;
    mutable AnnexInfo* annex_ = nullptr;
    mutable sub_t sub_        = 0;
};

/// `axm ptrn: type = value;`
class AxmDecl : public ValDecl {
public:
    class Alias : public Decl {
    public:
        Alias(Dbg dbg)
            : Decl(dbg.loc())
            , dbg_(dbg) {}

        Dbg dbg() const { return dbg_; }

        void bind(Scopes&, const AxmDecl*) const;
        std::ostream& stream(Tab&, std::ostream&) const override;

    private:
        Dbg dbg_;
        mutable Dbg full_;

        friend class AxmDecl;
    };

    AxmDecl(Loc loc, Dbg dbg, std::deque<Ptrs<Alias>>&& subs, Ptr<Expr>&& type, Dbg normalizer, Tok curry, Tok trip)
        : ValDecl(loc)
        , dbg_(dbg)
        , subs_(std::move(subs))
        , type_(std::move(type))
        , normalizer_(normalizer)
        , curry_(curry)
        , trip_(trip) {}

    Dbg dbg() const { return dbg_; }
    const auto& subs() const { return subs_; }
    size_t num_subs() const { return subs_.size(); }
    const auto& sub(size_t i) const { return subs_[i]; }
    const Expr* type() const { return type_.get(); }
    Dbg normalizer() const { return normalizer_; }
    Tok curry() const { return curry_; }
    Tok trip() const { return trip_; }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    std::deque<Ptrs<Alias>> subs_;
    Ptr<Expr> type_;
    Dbg normalizer_;
    Tok curry_, trip_;
    mutable sub_t offset_;
    mutable AnnexInfo* annex_ = nullptr;
    mutable const Def* mim_type_;
};

/// `.rec dbg: type = body`
class RecDecl : public ValDecl {
public:
    RecDecl(Loc loc, Dbg dbg, Ptr<Expr>&& type, Ptr<Expr>&& body, Ptr<RecDecl>&& next)
        : ValDecl(loc)
        , dbg_(dbg)
        , type_(std::move(type))
        , body_(std::move(body))
        , next_(std::move(next)) {}

    Dbg dbg() const { return dbg_; }
    const Expr* type() const { return type_.get(); }
    const Expr* body() const { return body_.get(); }
    const RecDecl* next() const { return next_.get(); }

    void bind(Scopes&) const override;
    virtual void bind_decl(Scopes&) const;
    virtual void bind_body(Scopes&) const;

    void emit(Emitter&) const override;
    virtual void emit_decl(Emitter&) const;
    virtual void emit_body(Emitter&) const;

    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Dbg dbg_;
    Ptr<Expr> type_;
    Ptr<Expr> body_;
    Ptr<RecDecl> next_;
    mutable AnnexInfo* annex_ = nullptr;
    mutable sub_t sub_        = 0;
};

/// One of:
/// * `λ   dom_0 ... dom_n-1 -> codom`
/// * `cn dom_0 ... dom_n-1`
/// * `fn dom_0 ... dom_n-1 -> codom`
/// * `lam dbg dom_0 ... dom_n-1 -> codom`
/// * `con dbg dom_0 ... dom_n-1`
/// * `fun dbg dom_0 ... dom_n-1 -> codom`
class LamDecl : public RecDecl {
public:
    class Dom : public PiExpr::Dom {
    public:
        Dom(Loc loc, Ptr<Ptrn>&& ptrn, Ptr<Expr>&& filter)
            : PiExpr::Dom(loc, std::move(ptrn))
            , filter_(std::move(filter)) {}

        bool is_implicit() const { return ptrn()->is_implicit(); }
        const Expr* filter() const { return filter_.get(); }

        void bind(Scopes&, bool quiet = false) const override;
        Lam* emit_value(Emitter&) const;
        std::ostream& stream(Tab&, std::ostream&) const override;

    private:
        Ptr<Expr> filter_;
        mutable Lam* lam_;

        friend class LamDecl;
    };

    LamDecl(Loc loc,
            Tok::Tag tag,
            bool is_external,
            Dbg dbg,
            Ptrs<Dom>&& doms,
            Ptr<Expr>&& codom,
            Ptr<Expr>&& body,
            Ptr<RecDecl>&& next)
        : RecDecl(loc, dbg, nullptr, std::move(body), std::move(next))
        , tag_(tag)
        , is_external_(is_external)
        , doms_(std::move(doms))
        , codom_(std::move(codom)) {
        assert(num_doms() != 0);
    }

    Tok::Tag tag() const { return tag_; }
    bool is_external() const { return is_external_; }
    const Ptrs<Dom>& doms() const { return doms_; }
    const Dom* dom(size_t i) const { return doms_[i].get(); }
    size_t num_doms() const { return doms_.size(); }
    const Expr* codom() const { return codom_.get(); }

    void bind_decl(Scopes&) const override;
    void bind_body(Scopes&) const override;
    void emit_decl(Emitter&) const override;
    void emit_body(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Tok::Tag tag_;
    bool is_external_;
    Ptrs<Dom> doms_;
    Ptr<Expr> codom_;
    mutable AnnexInfo* annex_ = nullptr;
    mutable sub_t sub_        = 0;
};

/// `cfun dbg dom -> codom`
class CDecl : public ValDecl {
public:
    CDecl(Loc loc, Tok::Tag tag, Dbg dbg, Ptr<Ptrn>&& dom, Ptr<Expr>&& codom)
        : ValDecl(loc)
        , tag_(tag)
        , dbg_(dbg)
        , dom_(std::move(dom))
        , codom_(std::move(codom)) {}

    Dbg dbg() const { return dbg_; }
    Tok::Tag tag() const { return tag_; }
    const Ptrn* dom() const { return dom_.get(); }
    const Expr* codom() const { return codom_.get(); }

    void bind(Scopes&) const override;
    void emit(Emitter&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    Tok::Tag tag_;
    Dbg dbg_;
    Ptr<Ptrn> dom_;
    Ptr<Expr> codom_;
};

/// rewrite rules
/// rule (x:T, y:T) : x+y => y+x (when );
/// all meta variables have to be introduced

class RuleDecl : public ValDecl {
public:
    RuleDecl(Loc loc, Dbg dbg, Ptr<Ptrn>&& var, Ptr<Expr>&& lhs, Ptr<Expr>&& rhs, Ptr<Expr>&& guard, bool is_normalizer)
        : ValDecl(loc)
        , dbg_(std::move(dbg))
        , var_(std::move(var))
        , lhs_(std::move(lhs))
        , rhs_(std::move(rhs))
        , guard_(std::move(guard))
        , is_normalizer_(is_normalizer) {}

    Dbg dbg() const { return dbg_; }
    const Ptrn* var() const { return var_.get(); }
    const Expr* lhs() const { return lhs_.get(); }
    const Expr* rhs() const { return rhs_.get(); }
    const Expr* guard() const { return guard_.get(); }
    bool is_normalizer() const { return is_normalizer_; }

    void bind(Scopes&) const override;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    void emit(Emitter&) const override;

    Dbg dbg_;
    Ptr<Ptrn> var_;
    Ptr<Expr> lhs_;
    Ptr<Expr> rhs_;
    Ptr<Expr> guard_;
    bool is_normalizer_;
};

/*
 * Module
 */

class Import : public Node {
public:
    Import(Loc loc, Tok::Tag tag, Dbg dbg, Ptr<Module>&& module)
        : Node(loc)
        , dbg_(dbg)
        , tag_(tag)
        , module_(std::move(module)) {}

    Dbg dbg() const { return dbg_; }
    Tok::Tag tag() const { return tag_; }
    const Module* module() const { return module_.get(); }

    void bind(Scopes&) const;
    void emit(Emitter&) const;
    std::ostream& stream(Tab&, std::ostream&) const;

    friend void swap(Import& i1, Import& i2) noexcept {
        using std::swap;
        swap(i1.dbg_, i2.dbg_);
        swap(i1.tag_, i2.tag_);
        swap(i1.module_, i2.module_);
    }

private:
    Dbg dbg_;
    Tok::Tag tag_;
    Ptr<Module> module_;
};

class Module : public Node {
public:
    Module(Loc loc, Ptrs<Import>&& imports, Ptrs<ValDecl>&& decls)
        : Node(loc)
        , imports_(std::move(imports))
        , decls_(std::move(decls)) {}

    const auto& implicit_imports() const { return implicit_imports_; }
    const auto& imports() const { return imports_; }
    const auto& decls() const { return decls_; }

    void add_implicit_imports(Ptrs<Import>&& imports) const { implicit_imports_ = std::move(imports); }

    void compile(AST&) const;
    void bind(AST&) const;
    void bind(Scopes&) const;
    void emit(AST&) const;
    void emit(Emitter&) const;
    std::ostream& stream(Tab&, std::ostream&) const override;

private:
    mutable Ptrs<Import> implicit_imports_;
    Ptrs<Import> imports_;
    Ptrs<ValDecl> decls_;
};

AST load_plugins(World&, View<Sym>);
inline AST load_plugins(World& w, View<std::string> plugins) {
    return load_plugins(w, Vector<Sym>(plugins.size(), [&](size_t i) { return w.sym(plugins[i]); }));
}
inline AST load_plugins(World& w, Sym sym) { return load_plugins(w, View<Sym>({sym})); }
inline AST load_plugins(World& w, const std::string& plugin) { return load_plugins(w, w.sym(plugin)); }

} // namespace mim::ast
