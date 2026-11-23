#include <charconv>

#include <deque>

#include "mim/def.h"
#include "mim/rewrite.h"

#include "mim/ast/ast.h"

#include "family.h"

using namespace std::literals;

namespace mim::ast {

using Tag = Tok::Tag;

/// The field or constructor names of a structural type and, per component or payload, those of its parts.
/// Derived from the AST alone, so neither hash-consing nor substitution can mix them up or lose them.
struct Shape {
    fe::SymMap<size_t> sym2idx;
    std::vector<const Shape*> ops; ///< `nullptr` if unknown.
};

class Emitter {
public:
    Emitter(AST& ast)
        : ast_(ast) {}

    AST& ast() const { return ast_; }
    World& world() { return ast().world(); }
    Driver& driver() { return world().driver(); }
    fe::Error& error() { return driver().error(); }

    /// @p name is *this* registration's own (unqualified) Dbg::sym; AnnexInfo::qualified turns it into the
    /// full `plugin.tag[.sub]` name. We must take it from the declaration rather than from Def::sym, since
    /// hash-consing can make several annexes share a single Def (e.g. `mod foo anx let bar = 23; anx let baz = 23;
    /// end`).
    void attach(AnnexInfo* annex, sub_t sub, Sym name, const Def* def) {
        if (annex)
            world().annexes().attach(annex->plugin_id(), annex->id.tag, sub, annex->qualified(driver(), name), def);
    }

    void attach_alias(AnnexInfo* annex, sub_t sub, Sym name) {
        world().annexes().attach_alias(annex->plugin_id(), annex->id.tag, sub, annex->qualified(driver(), name));
    }

    /// @name Names
    /// Field and constructor names follow the AST, since same-shaped types are one Def; see Shape.
    ///@{
    /// A declared @p variant prints under its name; an anonymous one spells out its cases, named as source does.
    /// Streams lazily so that it still renders under the PlainNames guard Error::msg formats within.
    auto variant_str(const Shape* shape, const Def* variant) {
        auto names = std::vector<std::string>();
        for (auto i : std::views::iota(size_t(0), variant->num_ops())) {
            auto syms = ctor_syms(shape, i);
            names.emplace_back(syms.empty() ? std::format("_{}", i) : syms.front());
        }
        return fe::StreamFn{[variant, names = std::move(names)](std::ostream& os) -> std::ostream& {
            if (auto sym = variant->sym(); sym && sym != '_') return std::print(os, "{}", variant), os;
            if (names.empty()) return os << "(|)";
            os << '(';
            for (auto sep = ""; auto i : std::views::iota(size_t(0), names.size())) {
                std::print(os, "{}| {}", sep, names[i]);
                if (auto op = variant->op(i); op != variant->world().sigma()) std::print(os, ": {}", op);
                sep = " ";
            }
            return os << ')';
        }};
    }

    std::optional<size_t> find_name(const Shape* shape, Dbg dbg) {
        if (!shape) return {};
        if (auto i = shape->sym2idx.find(dbg.sym()); i != shape->sym2idx.end()) return i->second;
        return {};
    }

    /// Explains why a name was not found in @p shape, if it is a Conflict; @p example selects by index instead.
    void note_conflict(const Shape* shape, std::string_view example) {
        if (shape == &Conflict)
            error()
                .n("names are lost where the arms of a `match` yield differently named results")
                .n("select it by index instead, as in `{}`", example);
    }

    void check_ctors(const VariantExpr* expr) {
        auto syms = fe::SymSet();
        for (auto ctor : expr->ctors())
            if (auto dbg = ctor->dbg(); !syms.emplace(dbg.sym()).second)
                error().e(dbg.loc(), "constructor `{}` declared twice", dbg);
    }

    /// Also `_i` - how the dumper spells case `i` - selects that case even if the variant names it otherwise.
    std::optional<size_t> find_ctor(const Shape* shape, const Def* variant, Dbg dbg) {
        if (auto i = find_name(shape, dbg); i && *i < variant->num_ops()) return i;
        auto sym  = dbg.sym();
        auto name = sym.view();
        if (name.size() < 2 || name[0] != '_' || (name.size() > 2 && name[1] == '0')) return {};
        size_t i       = 0;
        auto [end, ec] = std::from_chars(name.data() + 1, name.data() + name.size(), i);
        if (ec != std::errc() || end != name.data() + name.size() || i >= variant->num_ops()) return {};
        return i;
    }

    size_t ctor(const Shape* shape, const Def* variant, Dbg dbg) {
        if (auto i = find_ctor(shape, variant, dbg)) return *i;
        error().e(dbg.loc(), "variant `{}` has no constructor `{}`", variant_str(shape, variant), dbg);
        note_conflict(shape, "T#0");
        error().bail();
    }

    /// Every name case @p i goes by, sorted for a deterministic diagnostic; its index if it has none.
    std::string ctor_names(const Shape* shape, size_t i) {
        auto names = ctor_syms(shape, i);
        if (names.empty()) return std::format("`{}`", i);
        for (auto& name : names)
            name = std::format("`{}`", name);
        return std::format("{}", fe::Join(names, " / "));
    }
    ///@}

    /// @name Shapes
    /// Names of structural types follow the AST, since same-shaped types are one Def; `nullptr` if unknown.
    ///@{
    const Shape* tshape(const Expr* type) { return shape(type, true); }    ///< Of the type @p type denotes.
    const Shape* vshape(const Expr* value) { return shape(value, false); } ///< Of the type of @p value.

    /// Records @p shape for the binders of @p ptrn unless their annotations name them otherwise.
    void bind_shape(const Ptrn* ptrn, const Shape* shape) {
        if (auto tuple = ptrn->isa<TuplePtrn>()) {
            for (size_t i = 0, n = tuple->num_ptrns(); i != n; ++i) {
                auto op = !shape || shape == &Conflict ? shape : i < shape->ops.size() ? shape->ops[i] : nullptr;
                bind_shape(tuple->ptrn(i), op);
            }
        } else if (auto alias = ptrn->isa<AliasPtrn>()) {
            bind_shape(alias->ptrn(), shape);
            binder_shapes_[alias] = shape ? shape : ptrn_tshape(alias->ptrn());
        } else if (auto own = ptrn_tshape(ptrn)) {
            binder_shapes_[ptrn] = own;
        } else {
            binder_shapes_[ptrn] = shape;
        }
    }

    void bind_let(const Ptrn* ptrn, const Expr* value) {
        bind_shape(ptrn, vshape(value));
        if (ptrn->isa<IdPtrn>()) let_values_[ptrn] = value;
    }

    void add_axm(const AxmDecl::Name* name, const Expr* type) { axm_types_[name] = type; }

    /// The names of what `ret` binds after calling @p callee.
    const Shape* ret_shape(const Expr* callee) {
        if (auto path = callee->isa<PathExpr>())
            if (auto lam = target(path) ? target(path)->isa<LamDecl>() : nullptr)
                if (auto ret = lam->doms().back()->ret_type()) return tshape(ret);
        return nullptr;
    }
    ///@}

    /// @name Nominal scope
    /// Only the `mod` declaring a private `nom` - or one nested in it - may access its representation.
    ///@{
    void push_mod(const ModDecl* mod) { mods_.emplace_back(mod); }
    void pop_mod() { mods_.pop_back(); }
    fe::Vector<const ModDecl*> reset_mods() { return std::exchange(mods_, {}); }
    void restore_mods(fe::Vector<const ModDecl*> mods) { mods_ = std::move(mods); }

    struct NomInfo {
        const Nom* decl;
        const ModDecl* owner;
        const NomDecl* ast;
    };

    void add_nom(const Nom* nom, const NomDecl* decl) {
        key2nom_[nom->key()] = {nom, decl->rep_vis() == Vis::Priv ? mods_.back() : nullptr, decl};
    }

    /// The declared `nom` that @p nom instantiates and, if its representation is private, the `mod` owning it.
    NomInfo nom_decl(const Nom* nom) const {
        auto i = key2nom_.find(nom->key());
        return i != key2nom_.end() ? i->second : NomInfo{nom, nullptr, nullptr};
    }

    /// The names of the representation of @p nom - or of any instance of it.
    const Shape* nom_shape(const Nom* nom) {
        auto decl = nom_decl(nom).ast;
        return decl ? tshape(decl->body()) : nullptr;
    }

    bool within(const ModDecl* mod) const { return std::ranges::find(mods_, mod) != mods_.end(); }
    ///@}

private:
    std::vector<std::string> ctor_syms(const Shape* shape, size_t i) {
        auto names = std::vector<std::string>();
        if (shape)
            for (const auto& [sym, k] : shape->sym2idx)
                if (k == i) names.emplace_back(sym.str());
        std::ranges::sort(names);
        return names;
    }

    /// The names of the type @p expr denotes, if @p type, or else of the type of the value it denotes.
    const Shape* shape(const Expr* expr, bool type) {
        if (auto sigma = expr->isa<SigmaExpr>()) return type ? ptrn_tshape(sigma->ptrn()) : nullptr;
        if (auto tuple = expr->isa<TupleExpr>(); tuple && tuple->num_elems() == 1) return shape(tuple->elem(0), type);
        auto& memo = type ? tshapes_ : vshapes_;
        // A recursive type reaches itself again; its own Shape is already memoized by then.
        if (auto [i, ins] = memo.emplace(expr, nullptr); !ins) return i->second;
        auto res = type ? tshape_(expr) : vshape_(expr);
        // A binder that the answer depends on may not have been emitted yet, so only a known Shape is final.
        if (!res) return memo.erase(expr), nullptr;
        return memo[expr] = res;
    }

    const Shape* tshape_(const Expr* expr) {
        if (auto path = expr->isa<PathExpr>()) return path_shape(path, true);
        if (auto decl = expr->isa<DeclExpr>()) return tshape(decl->expr());
        if (expr->isa<AppExpr>() || InfixExpr::isa_op(Tag::T_at, expr)) return app_shape(expr, true);
        if (auto ex = InfixExpr::isa_op(Tag::T_extract, expr)) return component(tshape(ex->lhs()), ex->rhs());
        if (auto variant = expr->isa<VariantExpr>()) {
            auto& shape    = shapes_.emplace_back();
            tshapes_[expr] = &shape;
            for (size_t i = 0, n = variant->num_ctors(); i != n; ++i) {
                auto ctor = variant->ctor(i);
                shape.sym2idx.emplace(ctor->dbg().sym(), i);
                shape.ops.emplace_back(ctor->type() ? tshape(ctor->type()) : nullptr);
            }
            return &shape;
        }
        return nullptr;
    }

    const Shape* vshape_(const Expr* expr) {
        if (auto path = expr->isa<PathExpr>()) return path_shape(path, false);
        if (auto decl = expr->isa<DeclExpr>()) return vshape(decl->expr());
        if (auto ret = expr->isa<RetExpr>()) return vshape(ret->body());
        // The Shape of a `nom` is already that of its representation.
        if (auto prefix = expr->isa<PrefixExpr>()) return vshape(prefix->rhs());
        if (auto tuple = expr->isa<TupleExpr>()) {
            auto ops = std::vector<const Shape*>();
            for (auto elem : tuple->elems())
                ops.emplace_back(vshape(elem.get()));
            if (std::ranges::all_of(ops, [](auto op) { return op == nullptr; })) return nullptr;
            return &shapes_.emplace_back(fe::SymMap<size_t>(), std::move(ops));
        }
        if (expr->isa<AppExpr>() || InfixExpr::isa_op(Tag::T_at, expr)) return app_shape(expr, false);
        if (auto ex = InfixExpr::isa_op(Tag::T_extract, expr)) return component(vshape(ex->lhs()), ex->rhs());
        if (auto ins = InfixExpr::isa_op(Tag::T_arrow_l, expr)) {
            auto base = ins->lhs();
            while (auto ex = InfixExpr::isa_op(Tag::T_extract, base))
                base = ex->lhs();
            return vshape(base);
        }
        if (auto match = expr->isa<MatchExpr>()) {
            // An arm of unknown shape cannot contradict the others.
            const Shape* res = nullptr;
            for (auto arm : match->arms()) {
                auto shape = vshape(arm->body());
                if (!shape) continue;
                if (res && !same(res, shape)) return &Conflict;
                res = shape;
            }
            return res;
        }
        return nullptr;
    }

    /// The names of the type the annotation of @p ptrn denotes - field names for a TuplePtrn.
    const Shape* ptrn_tshape(const Ptrn* ptrn) {
        if (auto id = ptrn->isa<IdPtrn>()) return id->type() ? tshape(id->type()) : nullptr;
        if (auto grp = ptrn->isa<GrpPtrn>()) return ptrn_tshape(grp->id());
        if (auto alias = ptrn->isa<AliasPtrn>()) return ptrn_tshape(alias->ptrn());
        if (auto tuple = ptrn->isa<TuplePtrn>()) {
            if (auto i = ptrn_tshapes_.find(tuple); i != ptrn_tshapes_.end()) return i->second;
            auto& shape          = shapes_.emplace_back();
            ptrn_tshapes_[tuple] = &shape;
            for (size_t i = 0, n = tuple->num_ptrns(); i != n; ++i) {
                if (auto sym = tuple->field(i)) shape.sym2idx[sym] = i;
                shape.ops.emplace_back(ptrn_tshape(tuple->ptrn(i)));
            }
            return &shape;
        }
        return nullptr;
    }

    /// What @p path refers to, seen through `anx` aliases.
    static const Decl* target(const PathExpr* path) {
        auto decl = path->decl();
        while (auto alias = decl ? decl->isa<AliasDecl>() : nullptr)
            decl = alias->path()->decl();
        return decl;
    }

    const Shape* path_shape(const PathExpr* path, bool type) {
        auto decl = target(path);
        if (!decl) return nullptr;
        if (type) {
            if (auto i = let_values_.find(decl); i != let_values_.end()) return tshape(i->second);
            if (auto rec = decl->isa<RecDecl>(); rec && !rec->isa<LamDecl>()) return tshape(rec->body());
            return nullptr;
        }
        if (auto i = binder_shapes_.find(decl); i != binder_shapes_.end()) return i->second;
        if (auto ptrn = decl->isa<Ptrn>()) return ptrn_tshape(ptrn);
        return apply(decl, {}, false);
    }

    /// Peels the applications - `true` for an explicit one, `false` for `@` - off a callee that resolves to a Decl.
    const Shape* app_shape(const Expr* expr, bool type) {
        auto args   = fe::Vector<bool>();
        auto callee = expr;
        while (true)
            if (auto app = callee->isa<AppExpr>())
                args.emplace_back(true), callee = app->callee();
            else if (auto at = InfixExpr::isa_op(Tag::T_at, callee))
                args.emplace_back(false), callee = at->lhs();
            else
                break;
        std::ranges::reverse(args);
        if (auto path = callee->isa<PathExpr>())
            if (auto decl = target(path)) return apply(decl, args, type);
        // `T#C payload` constructs a value of `T`.
        if (auto ex = InfixExpr::isa_op(Tag::T_extract, callee); ex && !type && args.size() == 1)
            if (auto variant = tshape(ex->lhs()); variant && variant != &Conflict)
                if (auto path = ex->rhs()->isa<PathExpr>(); path && variant->sym2idx.contains(path->dbg().sym()))
                    return variant;
        return nullptr;
    }

    /// How many of the @p implicit doms @p args consume, trailing implicit ones included.
    static std::optional<size_t> num_consumed(std::span<const bool> implicit, std::span<const bool> args) {
        size_t k = 0, n = implicit.size();
        for (auto expl : args) {
            if (expl)
                while (k != n && implicit[k])
                    ++k;
            if (k == n) return {};
            ++k;
        }
        while (k != n && implicit[k])
            ++k;
        return k;
    }

    /// Codomains are names only, so a generic @p decl yields them for any type argument.
    const Shape* apply(const Decl* decl, std::span<const bool> args, bool type) {
        if (auto lam = decl->isa<LamDecl>()) {
            auto implicit = fe::Vector<bool>();
            for (auto dom : lam->doms())
                implicit.emplace_back(dom->is_implicit());
            if (num_consumed(implicit, args) != implicit.size()) return nullptr;
            if (type) {
                if (auto rec = lam->rec_sugar()) return tshape(rec->body());
                return lam->body() ? tshape(lam->body()) : nullptr;
            }
            if (!ISA(lam->tag(), C_DS)) return nullptr;
            if (lam->codom()) return tshape(lam->codom());
            return lam->body() ? vshape(lam->body()) : nullptr;
        }

        if (type) return nullptr;
        const Expr* fn_type = nullptr;
        if (auto i = axm_types_.find(decl); i != axm_types_.end()) fn_type = i->second;
        if (auto id = decl->isa<IdPtrn>()) fn_type = id->type();
        if (auto grp = decl->isa<GrpPtrn>()) fn_type = grp->id()->type();
        if (!fn_type) return nullptr;

        auto implicit = fe::Vector<bool>();
        auto codoms   = fe::Vector<const Expr*>{fn_type};
        for (auto t = fn_type;;) {
            if (auto pi = t->isa<PiExpr>(); pi && pi->codom())
                implicit.emplace_back(pi->dom()->is_implicit()), t = pi->codom();
            else if (auto arrow = InfixExpr::isa_op(Tag::T_arrow_r, t))
                implicit.emplace_back(false), t = arrow->rhs();
            else
                break;
            codoms.emplace_back(t);
        }
        // A partial application yields a function, whose type has no names.
        if (num_consumed(implicit, args) != implicit.size()) return nullptr;
        return tshape(codoms.back());
    }

    /// The part of @p shape that @p index selects; a Conflict stays one.
    const Shape* component(const Shape* shape, const Expr* index) {
        if (!shape || shape == &Conflict) return shape;
        auto i = std::optional<size_t>();
        if (auto path = index->isa<PathExpr>(); path && path->path()->dbgs().size() == 1) {
            if (auto j = shape->sym2idx.find(path->dbg().sym()); j != shape->sym2idx.end()) i = j->second;
        } else if (auto lit = index->isa<LitExpr>(); lit && lit->tag() == Tag::L_i) {
            i = lit->tok().lit_i().second;
        } else if (lit && lit->tag() == Tag::L_u) {
            i = lit->tok().lit_u();
        }
        return i && *i < shape->ops.size() ? shape->ops[*i] : nullptr;
    }

    /// Structural equality; a pair met again on a cycle is assumed equal.
    bool same(const Shape* a, const Shape* b) {
        auto assumed = fe::Vector<std::pair<const Shape*, const Shape*>>();
        return same(a, b, assumed);
    }

    bool same(const Shape* a, const Shape* b, fe::Vector<std::pair<const Shape*, const Shape*>>& assumed) {
        if (a == b) return true;
        if (!a || !b || a == &Conflict || b == &Conflict) return false;
        if (a->sym2idx != b->sym2idx || a->ops.size() != b->ops.size()) return false;
        if (std::ranges::contains(assumed, std::pair{a, b})) return true;
        assumed.emplace_back(a, b);
        for (size_t i = 0, n = a->ops.size(); i != n; ++i)
            if (!same(a->ops[i], b->ops[i], assumed)) return false;
        return true;
    }

    static inline const Shape Conflict{};

    AST& ast_;
    std::deque<Shape> shapes_;
    ankerl::unordered_dense::map<const Expr*, const Shape*> tshapes_, vshapes_;
    ankerl::unordered_dense::map<const TuplePtrn*, const Shape*> ptrn_tshapes_;
    ankerl::unordered_dense::map<const Decl*, const Shape*> binder_shapes_;
    ankerl::unordered_dense::map<const Decl*, const Expr*> let_values_, axm_types_;
    fe::Vector<const ModDecl*> mods_;
    ankerl::unordered_dense::map<flags_t, NomInfo> key2nom_;
};

/*
 * File
 */

void File::emit(AST& ast) const {
    auto emitter = Emitter(ast);
    emit(emitter);
}

void File::emit(Emitter& e) const {
    if (emitted_) return;
    emitted_ = true;

    // An imported file is emitted from within its importer's mods, but it is not nested in them.
    auto mods = e.reset_mods();
    e.push_mod(this);
    auto _ = e.world().push(loc());
    for (auto import : implicit_imports())
        import->emit(e);
    emit_decls(e);
    e.restore_mods(std::move(mods));
}

void UseDecl::emit(Emitter& e) const {
    if (file()) file()->emit(e);
}

/*
 * Ptrn::emit_value
 */

/// A wildcard binds nothing, so the Def it would name need not exist - and a dump would then have to name it.
static bool is_wildcard(const Ptrn* ptrn) {
    if (auto id = ptrn->isa<IdPtrn>()) return id->dbg().is_anon();
    if (auto tuple = ptrn->isa<TuplePtrn>())
        return std::ranges::all_of(tuple->ptrns(), [](auto p) { return is_wildcard(p.get()); });
    return false;
}

const Def* ErrorPtrn::emit_value(Emitter&, const Def* def) const { return def; }

static const Def* assign(Emitter& e, const Ptrn* ptrn, const Def* type, const Def* def) {
    if (auto res = Check::coerce(type, def)) return res->set(ptrn->dbg());

    e.error()
        .e(ptrn->loc(), "value bound to `{}` is not assignable to its annotated type", ptrn->dbg().sym())
        .n("expected `{}`, got `{}`", type, type_of(def))
        .bail();
}

const Def* IdPtrn::emit_value(Emitter& e, const Def* def) const {
    return def_ = type() ? assign(e, this, emit_type(e), def) : def->set(dbg());
}

const Def* IdDecl::emit(const Def* def) const { return def_ = def->set(dbg()); }

const Def* GrpPtrn::emit_value(Emitter& e, const Def* def) const { return def_ = assign(e, this, emit_type(e), def); }

const Def* AliasPtrn::emit_value(Emitter& e, const Def* def) const {
    return def_ = ptrn()->emit_value(e, def)->set(dbg());
}

const Def* Ptrn::emit_proj(Emitter& e, const Def* def, size_t n, size_t i) const {
    auto _ = e.world().push(loc());
    if (is_wildcard(this)) return emit_type(e), nullptr;
    return emit_value(e, def->proj(n, i));
}

/// Binds @p ptrn to the Var of @p mut - which a wildcard does not create.
static void emit_var(Emitter& e, const Ptrn* ptrn, Def* mut) {
    if (is_wildcard(ptrn))
        ptrn->emit_type(e);
    else
        ptrn->emit_value(e, mut->var());
}

const Def* TuplePtrn::emit_value(Emitter& e, const Def* def) const {
    auto _ = e.world().push(loc());
    emit_type(e);
    auto n = num_ptrns();
    // An unset Hole type has arity 1 until inference resolves it, so only a known arity may be rejected.
    if (auto t = def->type()->zonk(); n != 1 && !Hole::isa_unset(t))
        if (auto a = Lit::isa(t->arity()); a && *a != n)
            e.error().e(loc(), "pattern has {} components, but a value of type `{}` has {}", n, t, *a).bail();
    for (size_t i = 0; i != n; ++i)
        ptrn(i)->emit_proj(e, def, n, i);
    return def_ = def;
}

/*
 * Ptrn::emit_Type
 */

const Def* ErrorPtrn::emit_type(Emitter&) const { std::unreachable(); }

const Def* IdPtrn::emit_type(Emitter& e) const {
    auto _               = e.world().push(loc());
    return emitted_type_ = type() ? type()->emit(e) : e.world().mut_hole_type();
}

const Def* AliasPtrn::emit_type(Emitter& e) const { return ptrn()->emit_type(e); }

// The leading IdPtrn has just emitted the group's type.
const Def* GrpPtrn::emit_type(Emitter&) const { return id()->emitted_type_; }

const Def* TuplePtrn::emit_type(Emitter& e) const { return emit_body(e, {}); }

const Def* TuplePtrn::emit_body(Emitter& e, const Def* decl) const {
    auto _ = e.world().push(loc());
    auto n = num_ptrns();
    Sigma* sigma;
    if (decl)
        sigma = decl->as_mut<Sigma>();
    else
        sigma = e.world().mut_sigma(e.world().type_pending(), n);
    auto var = sigma->var();

    for (size_t i = 0; i != n; ++i) {
        sigma->set(i, ptrn(i)->emit_type(e));
        ptrn(i)->emit_proj(e, var, n, i);
    }

    auto imm = sigma->immutabilize();
    return imm ? imm : sigma;
}

const Def* TuplePtrn::emit_decl(Emitter& e, const Def* type) const {
    auto _ = e.world().push(loc());
    type   = type ? type : e.world().type_infer_level();
    return e.world().mut_sigma(type, num_ptrns());
}

/*
 * Expr
 */

const Def* Expr::emit(Emitter& e) const {
    auto _ = e.world().push(loc());
    return emit_(e);
}

const Def* Expr::emit_decl(Emitter& e, const Def* type) const {
    auto _ = e.world().push(loc());
    return emit_decl_(e, type);
}

void Expr::emit_body(Emitter& e, const Def* decl) const {
    auto _ = e.world().push(loc());
    emit_body_(e, decl);
}

const Def* ErrorExpr::emit_(Emitter&) const { std::unreachable(); }
const Def* HoleExpr::emit_(Emitter& e) const { return e.world().mut_hole(e.world().mut_hole_type()); }

const Def* PathExpr::emit_(Emitter& e) const {
    assert(decl());
    if (auto def = decl()->def()) return def;
    if (auto nom = decl()->isa<NomDecl>()) {
        if (auto def = nom->emit_ahead(e)) return def;
        e.error()
            .e(loc(), "nominal type `{}` refers to itself", dbg().sym())
            .n("a `nom` can only refer to itself through a sigma, a variant, or a function type")
            .bail();
    }
    e.error().e(loc(), "`{}` is a module and not a value", dbg().sym()).bail();
}

const Def* TypeExpr::emit_(Emitter& e) const {
    auto l = level()->emit(e);
    return e.world().type(l);
}

const Def* RuleExpr::emit_(Emitter& e) const {
    auto& w   = e.world();
    auto rule = w.mut_rule(w.rho(w.mut_hole_type(), w.mut_hole_type()));
    rule->set(name());
    rule->set_meta(meta() ? meta()->emit_type(e) : w.sigma());
    if (meta()) emit_var(e, meta(), rule);
    rule->set_lhs(lhs()->emit(e));
    rule->set_guard(guard()->emit(e));
    rule->set_rhs(rhs()->emit(e));
    if (auto var = rule->has_var(); var && rule->type()->has_free_var(var))
        e.error()
            .e(loc(), "the type of a rule must not depend on its meta variables")
            .n("its type is `{}`; make what it depends on a parameter of the family", rule->type()->zonk())
            .bail();
    if (auto rho = rule->type(); is_norm() && !Check::alpha(rho->dom(), rho->codom()))
        e.error()
            .e(loc(), "a normalizing rule must not change the type")
            .n("its left-hand side has type `{}` but its right-hand side has type `{}`", rho->dom(), rho->codom())
            .bail();
    return rule;
}

const Def* PrimaryExpr::emit_(Emitter& e) const {
    // clang-format off
    switch (tag()) {
        case Tag::K_Level: return e.world().type_level();
        case Tag::K_Nat:  return e.world().type_nat();
        case Tag::K_Idx:  return e.world().type_idx();
        case Tag::K_Bool: return e.world().type_bool();
        case Tag::K_ff:   return e.world().lit_ff();
        case Tag::K_tt:   return e.world().lit_tt();
        case Tag::K_i1:   return e.world().lit_i1();
        case Tag::K_i2:   return e.world().lit_i2();
        case Tag::K_i4:   return e.world().lit_i4();
        case Tag::K_i8:   return e.world().lit_i8();
        case Tag::K_i16:  return e.world().lit_i16();
        case Tag::K_i32:  return e.world().lit_i32();
        case Tag::K_i64:  return e.world().lit_i64();
        case Tag::K_I1:   return e.world().type_i1();
        case Tag::K_I2:   return e.world().type_i2();
        case Tag::K_I4:   return e.world().type_i4();
        case Tag::K_I8:   return e.world().type_i8();
        case Tag::K_I16:  return e.world().type_i16();
        case Tag::K_I32:  return e.world().type_i32();
        case Tag::K_I64:  return e.world().type_i64();
        case Tag::T_star: return e.world().type<0>();
        case Tag::T_box:  return e.world().type<1>();
        default: std::unreachable();
    }
    // clang-format on
}

/// If @p type is a `math.F` type of known precision/exponent, yields its bit width.
/// Note that libmim must not depend on the generated math plugin header, so lookup the Axm at runtime instead.
static std::optional<nat_t> isa_math_f(Emitter& e, const Def* type) {
    auto math_f = e.world().annex(e.world().sym("math.F"));
    if (auto app = type->zonk()->isa<App>(); math_f && app && app->callee() == math_f) {
        if (auto [p, ex] = app->arg()->projs<2>([](auto op) { return Lit::isa(op); }); p && ex) {
            if (*p == 10 && *ex == 5) return 16;
            if (*p == 23 && *ex == 8) return 32;
            if (*p == 52 && *ex == 11) return 64;
        }
    }
    return {};
}

/// A float Tok stores its value as mim::f64 bits; re-encode them for the width of the annotated type @p t.
static u64 encode_f(Emitter& e, [[maybe_unused]] Loc loc, const Def* t, u64 bits) {
    if (auto width = isa_math_f(e, t)) {
        auto val = std::bit_cast<f64>(bits);
        switch (*width) {
#if defined(__STDCPP_FLOAT16_T__)
            case 16: return std::bit_cast<u16>(f16(val));
#else
            case 16: e.error().e(loc, "16-bit floating-point literals are not supported on this platform").bail();
#endif
            case 32: return std::bit_cast<u32>(f32(val));
            default: break;
        }
    }
    return bits;
}

const Def* LitExpr::emit_(Emitter& e) const {
    auto t = type() ? type()->emit(e) : nullptr;
    // clang-format off
    switch (tag()) {
        case Tag::L_f:   return t ? e.world().lit(t, encode_f(e, loc(), t, tok().lit_u())) : e.world().lit_nat(tok().lit_u());
        case Tag::L_s:
            if (auto size = t ? Idx::isa_lit(t) : std::nullopt; size && *size != 0) {
                auto mag = -tok().lit_u();
                return e.world().lit(t, (*size - mag % *size) % *size);
            }
            [[fallthrough]];
        case Tag::L_u:   return t ? e.world().lit(t, tok().lit_u()) : e.world().lit_nat(tok().lit_u());
        case Tag::L_i:   { auto [size, val] = tok().lit_i(); return e.world().lit_idx(size, val); }
        case Tag::L_c:   return e.world().lit_i8(tok().lit_c());
        case Tag::L_str: return e.world().tuple(tok().sym());
        case Tag::T_bot: return t ? e.world().bot(t) : e.world().type_bot();
        case Tag::T_top: return t ? e.world().top(t) : e.world().type_top();
        default: std::unreachable();
    }
    // clang-format on
}

const Def* DeclExpr::emit_(Emitter& e) const {
    if (is_where())
        for (auto decl : decls() | std::views::reverse)
            decl->emit(e);
    else
        for (auto decl : decls())
            decl->emit(e);
    return expr()->emit(e);
}

const Def* InfixExpr::emit_decl_(Emitter& e, const Def* type) const {
    assert(op().isa(Tag::T_arrow_r));
    return pi_ = e.world().mut_pi(type, false);
}

void InfixExpr::emit_body_(Emitter& e, const Def*) const {
    pi_->set_dom(lhs()->emit(e));
    pi_->set_codom(rhs()->emit(e)); // TODO try to immutabilize
}

const Def* InfixExpr::emit_index(Emitter& e, const Def* tup, const Nom* nom) const {
    auto& w = e.world();
    // A simple path names a field of tup's Sigma before anything the binder resolved it to.
    if (auto path = rhs()->isa<PathExpr>(); path && path->path()->dbgs().size() == 1) {
        auto dbg   = path->dbg();
        auto shape = nom ? e.nom_shape(nom) : e.vshape(lhs());
        if (auto i = e.find_name(shape, dbg)) {
            if (auto decl = path->decl())
                e.error()
                    .w(dbg.loc(), "`{}` names both field {} of `{}` and a declaration in scope; using the field", dbg,
                       *i, tup->type())
                    .n(decl->loc(), "declaration here")
                    .n("parenthesize it, as in `#({})`, to extract by the declaration instead", dbg);
            // A 1-Sigma has collapsed to its field's type, whose arity is not the Sigma's.
            return w.lit(w.type_idx(shape->ops.size()), *i);
        }
        if (!path->decl()) {
            e.error().e(dbg.loc(), "cannot resolve field `{}` for extraction", dbg);
            e.note_conflict(shape, "t#0_2");
            e.error().bail();
        }
    }
    return rhs()->emit(e);
}

/// A private `nom`'s representation is confined to the `mod` that declared it; the type itself is not.
static void check_nom_scope(Emitter& e, Loc loc, const Nom* nom) {
    auto [decl, owner, _] = e.nom_decl(nom);
    if (!owner || e.within(owner)) return;
    auto where = owner->isa<File>() ? fe::format_cite("file `{}`", owner->loc().src->path().filename().string())
                                    : fe::format_cite("module `{}`", owner->dbg());
    e.error()
        .e(loc, "the representation of private nominal type `{}` is only accessible within {}", nom, where)
        .n(decl->loc(), "declared here")
        .bail();
}

/// Unwraps @p tup if it is a value of a `nom` and @p ex names one of its fields; `nullptr` otherwise.
static const Def* unwrap_field(Emitter& e, const InfixExpr* ex, const Def* tup) {
    auto nom  = tup->isa_type<Nom>();
    auto path = ex->rhs()->isa<PathExpr>();
    if (!nom || !path || path->path()->dbgs().size() != 1) return nullptr;
    if (!e.find_name(e.nom_shape(nom), path->dbg())) return nullptr;
    check_nom_scope(e, ex->loc(), nom);
    return e.world().struc(tup);
}

const Def* PrefixExpr::emit_(Emitter& e) const {
    auto def = rhs()->emit(e);
    switch (op().tag()) {
        case Tag::T_extract:
            if (auto nom = def->isa_type<Nom>()) {
                check_nom_scope(e, loc(), nom);
                return e.world().struc(def);
            }
            if (!def->zonk()->isa_type<Single>()) {
                auto& err = e.error().e(
                    loc(), "operand of prefix `#` is of type `{}` but must be of nominal or singleton type",
                    type_of(def));
                if (def->zonk()->isa<Nom>()) err.n("`{}` is a nominal type itself; `#` unwraps a value of it", def);
                err.bail();
            }
            return e.world().unwrap(def);
        default: std::unreachable();
    }
}

/// `T#C` or `T#i` selects case `C` or `i` of the Variant `T`: a constructor function - or its value for a `[]` payload.
/// If @p nom is given, `T` is its body and the constructor yields @p nom.
static const Def*
emit_ctor(Emitter& e, const Shape* shape, const Variant* variant, const Expr* index, const Nom* nom = nullptr) {
    auto& w = e.world();
    auto i  = std::optional<nat_t>();

    if (auto path = index->isa<PathExpr>(); path && path->path()->dbgs().size() == 1) {
        i = e.find_ctor(shape, variant, path->dbg());
        if (!i && !path->decl()) e.ctor(shape, variant, path->dbg());
    }
    if (!i) i = Lit::isa(index->emit(e));
    if (!i) e.error().e(index->loc(), "a case of a variant must be selected by name or by literal index").bail();
    if (*i >= variant->num_ops())
        e.error()
            .e(index->loc(), "variant `{}` has {} cases but case {} was requested", e.variant_str(shape, variant),
               variant->num_ops(), *i)
            .bail();

    auto type    = nom ? static_cast<const Def*>(nom) : variant;
    auto payload = variant->op(*i);
    if (payload == w.sigma()) return w.inj(type, *i, w.tuple());
    auto ctor = w.mut_lam(w.pi(payload, type));
    return ctor->set(true, w.inj(type, *i, ctor->var()));
}

const Def* InfixExpr::emit_(Emitter& e) const {
    auto& w = e.world();

    switch (op().tag()) {
        case Tag::T_extract: {
            auto tup = w.implicit_holes(lhs()->emit(e));
            if (auto variant = tup->isa<Variant>()) return emit_ctor(e, e.tshape(lhs()), variant, rhs());
            if (auto nom = tup->isa<Nom>()) {
                if (auto variant = nom->op()->isa<Variant>()) {
                    check_nom_scope(e, loc(), nom);
                    return emit_ctor(e, e.nom_shape(nom), variant, rhs(), nom);
                }
            }
            auto value = unwrap_field(e, this, tup);
            auto nom   = value ? tup->isa_type<Nom>() : nullptr;
            if (value) tup = value;
            return w.extract(tup, emit_index(e, tup, nom));
        }
        case Tag::T_arrow_l: {
            fe::Vector<const InfixExpr*> exs;
            auto base = lhs();
            while (auto ex = InfixExpr::isa_op(Tag::T_extract, base)) {
                exs.emplace_back(ex);
                base = ex->lhs();
            }

            if (exs.empty())
                e.error()
                    .e(lhs()->loc(), "expected `#` on the left-hand side of `{}`", Tok::tag2str(op().tag()))
                    .n("an update needs a component, as in `tuple#index {} value`", Tok::tag2str(op().tag()))
                    .bail();

            auto tup = base->emit(e);
            DefVec tups, idxs;
            fe::Vector<const Nom*> noms;
            for (auto ex : exs | std::views::reverse) {
                auto value = unwrap_field(e, ex, tup);
                noms.emplace_back(value ? tup->isa_type<Nom>() : nullptr);
                if (value) tup = value;
                auto idx = ex->emit_index(e, tup, noms.back());
                tups.emplace_back(tup);
                idxs.emplace_back(idx);
                tup = w.extract(tup, idx);
            }

            auto val = rhs()->emit(e);
            for (size_t i = tups.size(); i-- != 0;) {
                val = w.insert(tups[i], idxs[i], val);
                if (noms[i]) val = w.name(noms[i], val);
            }
            return val;
        }
        default: break;
    }

    auto c = callee() ? callee()->emit(e) : nullptr;
    auto l = lhs()->emit(e);
    auto r = rhs()->emit(e);

    switch (op().tag()) {
        case Tag::T_arrow_r: return w.pi(l, r);
        case Tag::T_leadsto: return w.rho(l, r);
        case Tag::T_fire: return w.fire(w.implicit_holes(l), r);
        case Tag::T_at: return w.app(l, r);
        case Tag::K_inj:
            r = w.implicit_holes(r);
            if (auto nom = r->isa<Nom>()) check_nom_scope(e, loc(), nom);
            return w.inj(r, l);
        default: return w.implicit_app(c, w.tuple({l, r})); // MIM_INFIX_SUGAR
    }
}

Lam* MatchExpr::Arm::emit(Emitter& e, const Def* dom) const {
    auto _ = e.world().push(loc());
    if (ptrn()) {
        auto dom_t = ptrn()->emit_type(e);
        if (!Check::alpha(dom_t, dom))
            e.error()
                .e(ptrn()->loc(), "pattern of type `{}` does not match the constructor's payload `{}`", dom_t, dom)
                .bail();
    }
    auto lam = e.world().mut_lam(e.world().pi(dom, e.world().mut_hole_type()));
    if (ptrn()) ptrn()->emit_value(e, lam->var());
    return lam->set(true, body()->emit(e));
}

const Def* MatchExpr::emit_variant(Emitter& e, const Def* scrutinee, const Variant* variant, const Nom* nom) const {
    auto shape = nom ? e.nom_shape(nom) : e.vshape(this->scrutinee());
    auto sel   = DefVec(variant->num_ops());
    for (auto arm : arms()) {
        auto i = size_t();
        if (auto index = arm->index()) {
            if (*index >= variant->num_ops())
                e.error()
                    .e(arm->loc(), "variant `{}` has {} cases but case {} was requested", e.variant_str(shape, variant),
                       variant->num_ops(), *index)
                    .bail();
            i = *index;
        } else if (auto ctor = arm->ctor()) {
            i = e.ctor(shape, variant, ctor);
        } else if (auto id = arm->ptrn()->isa<IdPtrn>(); id && !id->type()) {
            i = e.ctor(shape, variant, id->dbg());
        } else {
            e.error()
                .e(arm->ptrn()->loc(), "an arm of a match on variant `{}` must name a constructor",
                   e.variant_str(shape, variant))
                .bail();
        }
        if (sel[i]) {
            e.error().w(arm->loc(), "this arm is unreachable: constructor {} is already handled",
                        e.ctor_names(shape, i));
            continue;
        }
        if (auto ptrn = arm->ptrn(); ptrn && shape) e.bind_shape(ptrn, i < shape->ops.size() ? shape->ops[i] : nullptr);
        sel[i] = arm->emit(e, variant->op(i));
    }

    auto missing = std::vector<std::string>();
    for (size_t i = 0, n = sel.size(); i != n; ++i)
        if (!sel[i]) missing.emplace_back(e.ctor_names(shape, i));
    if (!missing.empty()) e.error().e(loc(), "match expression has no arm for {}", fe::Join(missing, ", ")).bail();

    auto ops = DefVec{scrutinee};
    ops.append_range(sel);
    return e.world().match(ops);
}

const Def* MatchExpr::emit_(Emitter& e) const {
    auto scrut = scrutinee()->emit(e);
    if (auto variant = scrut->isa_type<Variant>()) return emit_variant(e, scrut, variant, nullptr);
    if (auto nom = scrut->isa_type<Nom>())
        if (auto variant = nom->op()->isa<Variant>()) {
            check_nom_scope(e, loc(), nom);
            return emit_variant(e, e.world().struc(scrut), variant, nom);
        }

    e.error()
        .e(scrutinee()->loc(), "scrutinee of a match expression must be of variant type")
        .n("but it has type `{}`", type_of(scrut))
        .bail();
}

void PiExpr::Dom::emit_type(Emitter& e) const {
    // Created before the push: the Pi belongs to the whole function type, not just to this Dom.
    pi_        = decl_ ? decl_ : e.world().mut_pi(e.world().type_pending(), is_implicit());
    auto _     = e.world().push(loc());
    auto dom_t = ptrn()->emit_type(e);

    if (ret_type()) {
        auto sigma = e.world().mut_sigma(2);
        auto var   = sigma->var();
        sigma->set(0, dom_t);
        ptrn()->emit_proj(e, var, 2, 0);
        auto ret_t = e.world().cn(ret_type()->emit(e));
        sigma->set(1, ret_t);

        auto imm = sigma->immutabilize();
        pi_->set_dom(imm ? imm : sigma);
    } else {
        pi_->set_dom(dom_t);
        emit_var(e, ptrn(), pi_);
    }
}

const Def* PiExpr::emit_decl_(Emitter& e, const Def* type) const {
    return dom()->decl_ = e.world().mut_pi(type, dom()->is_implicit());
}

void PiExpr::emit_body_(Emitter& e, const Def*) const { emit(e); }

const Def* PiExpr::emit_(Emitter& e) const {
    dom()->emit_type(e);
    auto cod = codom() ? codom()->emit(e) : e.world().type_bot();
    auto pi  = dom()->pi_->set_codom(cod);
    if (auto imm = pi->immutabilize()) return imm;
    return pi;
}

const Def* LamExpr::emit_decl_(Emitter& e, const Def*) const { return lam()->emit_decl(e), lam()->def(); }
void LamExpr::emit_body_(Emitter& e, const Def*) const { lam()->emit_body(e); }

const Def* LamExpr::emit_(Emitter& e) const {
    auto res = emit_decl(e, {});
    emit_body(e, {});
    return res;
}

const Def* AppExpr::emit_(Emitter& e) const {
    auto c = callee()->emit(e);
    auto a = arg()->emit(e);
    return e.world().implicit_app(c, a);
}

const Def* RetExpr::emit_(Emitter& e) const {
    auto& w = e.world();
    auto c  = callee()->emit(e);
    if (auto pi = c->type()->isa<Pi>())
        if (auto cn = pi->ret_pi()) {
            auto a = arg()->emit(e);
            // A dependent domain's ret type refers to the other arguments, so instantiate it with them.
            if (auto sigma = pi->dom()->isa_mut<Sigma>()) {
                auto n    = sigma->num_ops();
                auto args = DefVec(n, [&](size_t i) {
                    return i + 1 == n ? w.bot(sigma->op(i)) : n == 2 ? a : a->proj(n - 1, i);
                });
                if (auto inst = sigma->reduce(w.tuple(args)).back()->isa<Pi>()) cn = inst;
            }
            auto con  = w.mut_lam(cn);
            auto pair = w.tuple({a, con});
            auto app  = w.app(c, pair);
            e.bind_shape(ptrn(), e.ret_shape(callee()));
            ptrn()->emit_value(e, con->var());
            con->set(false, body()->emit(e));
            return app;
        }

    e.error()
        .e(callee()->loc(), "callee of a `ret` expression must be a returning continuation, but `{}` has type `{}`", c,
           c->type())
        .bail();
}

const Def* SigmaExpr::emit_decl_(Emitter& e, const Def* type) const { return ptrn()->emit_decl(e, type); }
void SigmaExpr::emit_body_(Emitter& e, const Def* decl) const { ptrn()->emit_body(e, decl); }
const Def* SigmaExpr::emit_(Emitter& e) const { return ptrn()->emit_type(e); }

DefVec VariantExpr::emit_payloads(Emitter& e) const {
    return DefVec(num_ctors(), [&](size_t i) -> const Def* {
        auto type = ctor(i)->type();
        return type ? type->emit(e) : e.world().sigma();
    });
}

const Def* VariantExpr::emit_(Emitter& e) const {
    auto variant = e.world().variant(emit_payloads(e));
    e.check_ctors(this);
    return variant;
}

const Def* VariantExpr::emit_decl_(Emitter& e, const Def* type) const {
    return e.world().mut_variant(type, num_ctors());
}

void VariantExpr::emit_body_(Emitter& e, const Def* decl) const {
    auto variant = decl->as_mut<Variant>();
    variant->set(emit_payloads(e));
    e.check_ctors(this);
}

const Def* TupleExpr::emit_(Emitter& e) const {
    DefVec elems(num_elems(), [&](size_t i) { return elem(i)->emit(e); });
    return e.world().tuple(elems);
}

const Def* SeqExpr::emit_(Emitter& e) const {
    auto s = shape()->emit(e);
    if (auto lit_s = Lit::isa(s); lit_s && *lit_s == 0) return e.world().prod(is_pack());

    if (index()->dbg().is_anon()) { // immutable
        auto b = body()->emit(e);
        return e.world().seq(is_pack(), s, b);
    }

    auto a = e.world().mut_arr(e.world().type_pending());
    a->set_shape(s);

    if (is_pack()) {
        auto p = e.world().mut_pack(a);
        p->set_shape(s);
        auto var = p->var();
        index()->emit(var);
        auto b = body()->emit(e);
        p->set_body(b);
        auto arr_b = b->type();
        if (auto pvar = var->isa<Var>())
            // Use array var in array body instead of pack var
            arr_b = VarRewriter(pvar, a->var()).rewrite(arr_b);
        a->set_body(arr_b);
        return e.world().fuse(p);
    } else {
        auto var = a->var();
        index()->emit(var);
        a->set_body(body()->emit(e));
        return e.world().fuse(a);
    }
}

const Def* SingleExpr::emit_(Emitter& e) const {
    auto def = body()->emit(e);
    return is_wrap() ? e.world().wrap(def) : e.world().single(def);
}

/*
 * Decl
 */

void AxmDecl::emit(Emitter& e) const {
    auto _                 = e.world().push(loc());
    auto type              = type_->emit(e);
    auto [n_curry, n_trip] = Axm::infer_curry_and_trip(type);
    // An Axm cannot be replaced by the inhabitant, which all other values of a singleton type collapse to.
    if (e.world().inhabitant(type))
        e.error().e(type_->loc(), "axiom of singleton type `{}` carries no information", type).bail();
    if (curry_) {
        if (curry_.lit_u() > n_curry)
            e.error().e(curry_.loc(), "curry counter cannot be greater than {}", n_curry).bail();
        else
            n_curry = curry_.lit_u();
    }

    if (trip_) {
        if (trip_.lit_u() > n_curry)
            e.error().e(trip_.loc(), "trip counter cannot be greater than curry counter {}", (int)n_curry).bail();
        else
            n_trip = trip_.lit_u();
    }

    for (auto name : names()) {
        auto annex = name->annex_;
        if (!annex) continue; // binding failed
        auto plugin = annex->plugin_id();
        auto tag    = annex->id.tag;
        auto sub    = name->sub_;
        auto norm   = e.driver().normalizer(plugin, tag, sub);
        auto sym    = annex->qualified(e.driver(), name->dbg().sym());
        auto axm    = e.world().axm(norm, n_curry, n_trip, type, plugin, tag, sub)->set(sym);
        name->def_  = axm;
        e.add_axm(name.get(), type_.get());
        e.world().annexes().attach(plugin, tag, sub, sym, axm);
        for (auto alias : name->aliases())
            e.attach_alias(annex, sub, alias.sym());
    }
}

void AliasDecl::emit(Emitter& e) const {
    if (!annex_) return; // skip emit if binding failed
    auto target = path()->decl();
    def_        = target->def();
    if (foreign_)
        e.world().annexes().attach_alias(annex_->plugin_id(), annex_->id.tag, sub_, foreign_);
    else
        e.attach_alias(annex_, sub_, dbg().sym());
}

void ModDecl::emit_decls(Emitter& e) const {
    for (auto decl : decls()) {
        decl->emit(e);
        e.world().solve_levels();
    }
}

void ModDecl::emit(Emitter& e) const {
    e.push_mod(this);
    emit_decls(e);
    e.pop_mod();
}

void LetDecl::emit(Emitter& e) const {
    auto _ = e.world().push(loc());
    auto v = value()->emit(e);
    e.bind_let(ptrn(), value());
    def_ = ptrn()->emit_value(e, v);
    if (auto id = ptrn()->isa<IdPtrn>()) e.attach(id->annex_, id->sub_, id->dbg().sym(), def_);
}

void RecDecl::emit(Emitter& e) const {
    emit_decl(e);
    emit_body(e);
}

void MutualDecl::emit(Emitter& e) const {
    for (auto decl : decls())
        decl->emit_decl(e);
    for (auto decl : decls())
        decl->emit_body(e);
}

void RecDecl::emit_decl(Emitter& e) const {
    auto _ = e.world().push(loc());
    def_   = body()->emit_decl(e, e.world().type_infer_level());
    def_->set(dbg().sym());
}

void RecDecl::emit_body(Emitter& e) const {
    auto _ = e.world().push(loc());
    body()->emit_body(e, def_);
    // TODO immutabilize?
    e.attach(annex_, sub_, dbg().sym(), def_);
}

void RecDecl::emit_sugar(Emitter& e, Lam* lam) const {
    emit_decl(e);
    lam->set_body(def_);
    emit_body(e);
}

void NomDecl::emit_decl(Emitter& e) const {
    if (!has_mut_body()) return;
    auto _ = e.world().push(loc());
    emit_nom(e, body()->emit_decl(e, e.world().type_infer_level()));
}

void NomDecl::emit_body(Emitter& e) const {
    auto _ = e.world().push(loc());
    if (has_mut_body()) {
        body()->emit_body(e, def_->as<Nom>()->op());
    } else {
        if (def_) return; // see emit_ahead
        emitting_ = true;
        emit_nom(e, body()->emit(e));
    }
    e.attach(annex_, sub_, dbg().sym(), def_);
}

const Def* NomDecl::emit_ahead(Emitter& e) const {
    if (!has_mut_body() && !emitting_) emit_body(e);
    return def_;
}

void NomDecl::emit_sugar(Emitter& e, Lam* lam) const {
    // `I dom` then resolves to this very instance as an application of a Lam to its own Var.
    emit_decl(e);
    lam->set_body(emit_ahead(e));
    emit_body(e);
}

void NomDecl::emit_nom(Emitter& e, const Def* body) const {
    // World::dump finds a file's annexes by their qualified sym.
    auto sym = annex_ ? annex_->qualified(e.driver(), dbg().sym()) : dbg().sym();
    auto nom = e.world().nom(e.world().next_gid(), body);
    nom->set(sym);
    e.add_nom(nom, this);
    def_ = nom;
}

Lam* LamDecl::Dom::emit_value(Emitter& e) const {
    // Created before the push: the Lam belongs to the whole declaration, not just to this Dom.
    lam_   = e.world().mut_lam(pi_);
    auto _ = e.world().push(loc());

    if (ret()) {
        auto var = lam_->var();
        ptrn()->emit_proj(e, var, 2, 0);
        ret()->emit(var->proj(2, 1));
    } else {
        emit_var(e, ptrn(), lam_);
    }

    return lam_;
}

void LamDecl::emit_decl(Emitter& e) const {
    auto _      = e.world().push(loc());
    bool is_cps = !ISA(tag_, C_DS);

    // Iterate over all doms: Build a Lam for curr dom, by first building a curried Pi for the remaining doms.
    for (size_t i = 0, n = num_doms(); i != n; ++i) {
        for (auto dom : doms() | std::views::drop(i))
            dom->emit_type(e);

        auto cod = codom() ? codom()->emit(e) : is_cps ? e.world().type_bot() : e.world().mut_hole_type();
        for (auto dom : doms() | std::views::drop(i) | std::views::reverse)
            cod = dom->pi_->set_codom(cod);

        auto cur = dom(i);
        auto lam = cur->emit_value(e);
        if (auto filter = cur->filter()) {
            auto _filter = e.world().push(filter->loc());
            lam->set_filter(filter->emit(e));
        } else {
            lam->set_filter(i + 1 == n && is_cps ? e.world().lit_ff() : e.world().lit_tt());
        }

        if (i == 0)
            def_ = lam->set(dbg().sym());
        else
            dom(i - 1)->lam_->set_body(lam);
    }
}

void LamDecl::emit_body(Emitter& e) const {
    if (!body()) return; // extern forward declaration: the implementation lives in a native translation unit

    auto _ = e.world().push(loc());
    {
        auto _body = e.world().push(body()->loc());
        auto lam   = doms().back()->lam_;
        if (auto rec = rec_sugar())
            rec->emit_sugar(e, lam);
        else
            lam->set_body(body()->emit(e));
    }

    // rewrite holes
    for (size_t i = 0, n = num_doms(); i != n; ++i) {
        auto rw  = VarRewriter(e.world());
        auto lam = dom(i)->lam_;
        auto pi  = lam->type()->as_mut<Pi>();
        for (auto dom : doms() | std::views::drop(i)) {
            // A Lam's Var that was never created cannot occur in the codomain.
            if (auto var = pi->has_var())
                if (auto lam_var = dom->lam_->has_var()) rw.add(lam_var, var);
            auto cod = pi->codom();
            if (!cod || !cod->isa_mut<Pi>()) break;
            pi = cod->as_mut<Pi>();
        }

        if (auto cod = pi->codom(); cod && cod->has_hole()) pi->set(pi->dom(), rw.rewrite(cod));
    }

    for (auto dom : doms() | std::views::reverse) {
        if (auto imm = dom->pi_->immutabilize()) {
            auto f = dom->lam_->filter();
            auto b = dom->lam_->body();
            dom->lam_->unset()->set_type(imm)->as<Lam>()->set(f, b);
        }
    }

    if (is_extern()) {
        auto lam = doms().front()->lam_;
        if (!lam->is_closed())
            e.error()
                .e(loc(),
                   "external function `{}` is not closed: its inferred type escapes into the scope of `{}`. This "
                   "usually means an unannotated parameter's type could only be inferred to depend on a variable bound "
                   "in an inner/sibling scope; add an explicit type annotation to the offending parameter.",
                   dbg().sym(), lam->free_vars().min()->binder()->sym())
                .bail();
        if (auto prev = e.world().externals()[dbg().sym()])
            e.error()
                .e(loc(), "external function `{}` is already defined", dbg().sym())
                .n(prev->loc(), "previous definition here")
                .bail();
        lam->externalize();
    }
    e.attach(annex_, sub_, dbg().sym(), def_);
}

} // namespace mim::ast
