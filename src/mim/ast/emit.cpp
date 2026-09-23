#include "mim/def.h"
#include "mim/rewrite.h"

#include "mim/ast/ast.h"

#include "family.h"

using namespace std::literals;

namespace mim::ast {

using Tag = Tok::Tag;

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
    /// hash-consing can make several annexes share a single Def (e.g. `mod foo { anx let bar = 23; anx let baz = 23;
    /// }`).
    void attach(AnnexInfo* annex, sub_t sub, Sym name, const Def* def) {
        if (annex)
            world().annexes().attach(annex->plugin_id(), annex->id.tag, sub, annex->qualified(driver(), name), def);
    }

    /// @name Names
    /// Field and constructor names live on the Def, so same-shaped types share one table.
    ///@{
    void add_names(const Def* def, const fe::SymMap<size_t>& sym2idx) {
        auto& names = def2sym2idx_[def];
        for (auto [sym, i] : sym2idx)
            if (auto [j, ins] = names.emplace(sym, i); !ins && j->second != i) j->second = Ambiguous;
    }

    std::optional<size_t> find_name(const Def* def, Dbg dbg) {
        auto i = def2sym2idx_.find(def);
        if (i == def2sym2idx_.end()) return {};
        auto j = i->second.find(dbg.sym());
        if (j == i->second.end()) return {};
        if (j->second == Ambiguous) {
            if (def->isa<Variant>())
                error()
                    .e(dbg.loc(), "constructor `{}` is ambiguous in variant `{}`", dbg, def)
                    .n("variants of the same shape name it at different positions")
                    .n("select the case by index instead, as in `T#0`")
                    .bail();
            error()
                .e(dbg.loc(), "field `{}` is ambiguous in `{}`", dbg, def)
                .n("sigmas of the same shape name it at different positions")
                .n("select the field by index instead, as in `t#0_2`")
                .bail();
        }
        return j->second;
    }

    void add_ctors(const Def* variant, const VariantExpr* expr) {
        auto sym2idx = fe::SymMap<size_t>();
        for (size_t i = 0, n = expr->num_ctors(); i != n; ++i) {
            auto dbg = expr->ctor(i)->dbg();
            if (!sym2idx.emplace(dbg.sym(), i).second) error().e(dbg.loc(), "constructor `{}` declared twice", dbg);
        }
        add_names(variant, sym2idx);
    }

    size_t ctor(const Def* variant, Dbg dbg) {
        if (auto i = find_name(variant, dbg)) return *i;
        error().e(dbg.loc(), "variant `{}` has no constructor `{}`", variant, dbg).bail();
    }

    /// Every name case @p i goes by, sorted for a deterministic diagnostic; `#i` if it has none.
    std::string ctor_names(const Def* variant, size_t i) {
        auto names = std::vector<std::string>();
        if (auto j = def2sym2idx_.find(variant); j != def2sym2idx_.end())
            for (const auto& [sym, k] : j->second)
                if (k == i) names.emplace_back(std::format("`{}`", sym));
        if (names.empty()) return std::format("`#{}`", i);
        std::ranges::sort(names);
        return std::format("{}", fe::Join(names, " / "));
    }
    ///@}

private:
    static constexpr size_t Ambiguous = size_t(-1);

    AST& ast_;
    absl::node_hash_map<const Def*, fe::SymMap<size_t>, GIDHash<const Def*>> def2sym2idx_;
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

    auto _ = e.world().push(loc());
    for (auto import : implicit_imports())
        import->emit(e);
    emit_decls(e);
}

void UseDecl::emit(Emitter& e) const {
    if (file()) file()->emit(e);
}

/*
 * Ptrn::emit_value
 */

const Def* ErrorPtrn::emit_value(Emitter&, const Def* def) const { return def; }

const Def* IdPtrn::emit_value(Emitter& e, const Def* def) const {
    emit_type(e);
    return def_ = def->set(dbg());
}

const Def* GrpPtrn::emit_value(Emitter&, const Def* def) const { return def_ = def->set(dbg()); }

const Def* AliasPtrn::emit_value(Emitter& e, const Def* def) const {
    return def_ = ptrn()->emit_value(e, def)->set(dbg());
}

const Def* Ptrn::emit_proj(Emitter& e, const Def* def, size_t n, size_t i) const {
    auto _ = e.world().push(loc());
    return emit_value(e, def->proj(n, i));
}

const Def* TuplePtrn::emit_value(Emitter& e, const Def* def) const {
    auto _ = e.world().push(loc());
    emit_type(e);
    for (size_t i = 0, n = num_ptrns(); i != n; ++i)
        ptrn(i)->emit_proj(e, def, n, i);
    return def_ = def;
}

/*
 * Ptrn::emit_Type
 */

const Def* ErrorPtrn::emit_type(Emitter&) const { fe::unreachable(); }

const Def* IdPtrn::emit_type(Emitter& e) const {
    auto _ = e.world().push(loc());
    return type() ? type()->emit(e) : e.world().mut_hole_type();
}

const Def* AliasPtrn::emit_type(Emitter& e) const { return ptrn()->emit_type(e); }

const Def* GrpPtrn::emit_type(Emitter& e) const { return id()->emit_type(e); }

const Def* TuplePtrn::emit_type(Emitter& e) const { return emit_body(e, {}); }

const Def* TuplePtrn::emit_body(Emitter& e, const Def* decl) const {
    auto _ = e.world().push(loc());
    auto n = num_ptrns();
    Sigma* sigma;
    if (decl) {
        sigma = decl->as_mut<Sigma>();
    } else {
        auto type = e.world().type_infer_univ();
        sigma     = e.world().mut_sigma(type, n);
    }
    auto var     = sigma->var();
    auto sym2idx = fe::SymMap<size_t>();

    for (size_t i = 0; i != n; ++i) {
        sigma->set(i, ptrn(i)->emit_type(e));
        ptrn(i)->emit_proj(e, var, n, i);
        if (auto p = ptrn(i); (p->isa<IdPtrn>() || p->isa<GrpPtrn>()) && !p->dbg().is_anon())
            sym2idx[p->dbg().sym()] = i;
    }

    e.add_names(sigma, sym2idx);
    auto imm = sigma->immutabilize();
    // A 1-Sigma collapses to its field's type, which must not inherit the name.
    if (imm && n > 1) e.add_names(imm, sym2idx);
    return imm ? imm : sigma;
}

const Def* TuplePtrn::emit_decl(Emitter& e, const Def* type) const {
    auto _ = e.world().push(loc());
    type   = type ? type : e.world().type_infer_univ();
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

const Def* ErrorExpr::emit_(Emitter&) const { fe::unreachable(); }
const Def* HoleExpr::emit_(Emitter& e) const { return e.world().mut_hole_type(); }

const Def* PathExpr::emit_(Emitter& e) const {
    assert(decl());
    if (auto def = decl()->def()) return def;
    e.error().e(loc(), "`{}` is a module and not a value", dbg().sym()).bail();
}

const Def* TypeExpr::emit_(Emitter& e) const {
    auto l = level()->emit(e);
    return e.world().type(l);
}

const Def* RuleExpr::emit_(Emitter& e) const {
    auto m = dom()->emit(e);
    return e.world().reform(m);
}

const Def* PrimaryExpr ::emit_(Emitter& e) const {
    // clang-format off
    switch (tag()) {
        case Tag::K_Univ: return e.world().univ();
        case Tag::K_Nat:  return e.world().type_nat();
        case Tag::K_Idx:  return e.world().type_idx();
        case Tag::K_Bool: return e.world().type_bool();
        case Tag::K_ff:   return e.world().lit_ff();
        case Tag::K_tt:   return e.world().lit_tt();
        case Tag::K_i1:   return e.world().lit_i1();
        case Tag::K_i8:   return e.world().lit_i8();
        case Tag::K_i16:  return e.world().lit_i16();
        case Tag::K_i32:  return e.world().lit_i32();
        case Tag::K_i64:  return e.world().lit_i64();
        case Tag::K_I1:   return e.world().type_i1();
        case Tag::K_I8:   return e.world().type_i8();
        case Tag::K_I16:  return e.world().type_i16();
        case Tag::K_I32:  return e.world().type_i32();
        case Tag::K_I64:  return e.world().type_i64();
        case Tag::T_star: return e.world().type<0>();
        case Tag::T_box:  return e.world().type<1>();
        default: fe::unreachable();
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
        case Tag::L_u:   return t ? e.world().lit(t, tok().lit_u()) : e.world().lit_nat(tok().lit_u());
        case Tag::L_i:   { auto [size, val] = tok().lit_i(); return e.world().lit_idx(size, val); }
        case Tag::L_c:   return e.world().lit_i8(tok().lit_c());
        case Tag::L_str: return e.world().tuple(tok().sym());
        case Tag::T_bot: return t ? e.world().bot(t) : e.world().type_bot();
        case Tag::T_top: return t ? e.world().top(t) : e.world().type_top();
        default: fe::unreachable();
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

/// `a ∪ b ∪ c` is one n-ary Join, so flatten the left spine the left-associative parse built.
static void emit_union(Emitter& e, const Expr* expr, DefVec& types) {
    if (auto infix = InfixExpr::isa_op(Tag::T_union, expr)) {
        emit_union(e, infix->lhs(), types);
        types.emplace_back(infix->rhs()->emit(e));
    } else {
        types.emplace_back(expr->emit(e));
    }
}

const Def* InfixExpr::emit_index(Emitter& e, const Def* tup) const {
    auto& w = e.world();
    // A simple path names a field of tup's Sigma before anything the binder resolved it to.
    if (auto path = rhs()->isa<PathExpr>(); path && path->path()->dbgs().size() == 1) {
        auto dbg = path->dbg();
        if (auto i = e.find_name(tup->type(), dbg)) return w.lit(w.type_idx(tup->type()->arity()), *i);
        if (!path->decl()) e.error().e(dbg.loc(), "cannot resolve field `{}` for extraction", dbg).bail();
    }
    return rhs()->emit(e);
}

const Def* PrefixExpr::emit_(Emitter& e) const {
    auto def = rhs()->emit(e);
    switch (op().tag()) {
        case Tag::T_extract: return e.world().unwrap(def);
        default: fe::unreachable();
    }
}

/// `T#C` or `T#i` selects case `C` or `i` of the Variant `T`: a constructor function - or its value for a `[]` payload.
static const Def* emit_ctor(Emitter& e, const Variant* variant, const Expr* index) {
    auto& w = e.world();
    auto i  = std::optional<nat_t>();

    if (auto path = index->isa<PathExpr>(); path && path->path()->dbgs().size() == 1) {
        i = e.find_name(variant, path->dbg());
        if (!i && !path->decl())
            e.error().e(path->dbg().loc(), "variant `{}` has no constructor `{}`", variant, path->dbg()).bail();
    }
    if (!i) i = Lit::isa(index->emit(e));
    if (!i) e.error().e(index->loc(), "a case of a variant must be selected by name or by literal index").bail();
    if (*i >= variant->num_ops())
        e.error()
            .e(index->loc(), "variant `{}` has {} cases but case {} was requested", variant, variant->num_ops(), *i)
            .bail();

    auto payload = variant->op(*i);
    if (payload == w.sigma()) return w.inj(variant, *i, w.tuple());
    auto ctor = w.mut_lam(w.pi(payload, variant));
    return ctor->set(true, w.inj(variant, *i, ctor->var()));
}

const Def* InfixExpr::emit_(Emitter& e) const {
    auto& w = e.world();

    switch (op().tag()) {
        case Tag::T_union: {
            DefVec types;
            emit_union(e, this, types);
            return w.join(types);
        }
        case Tag::T_extract: {
            auto tup = lhs()->emit(e);
            if (auto variant = tup->isa<Variant>()) return emit_ctor(e, variant, rhs());
            return w.extract(tup, emit_index(e, tup));
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
            for (auto ex : exs | std::views::reverse) {
                auto idx = ex->emit_index(e, tup);
                tups.emplace_back(tup);
                idxs.emplace_back(idx);
                tup = w.extract(tup, idx);
            }

            auto val = rhs()->emit(e);
            for (size_t i = tups.size(); i-- != 0;)
                val = w.insert(tups[i], idxs[i], val);
            return val;
        }
        default: break;
    }

    auto c = callee() ? callee()->emit(e) : nullptr;
    auto l = lhs()->emit(e);
    auto r = rhs()->emit(e);

    switch (op().tag()) {
        case Tag::T_arrow_r: return w.pi(l, r);
        case Tag::T_at: return w.app(l, r);
        case Tag::K_inj: return w.inj(r, l);
        default: return w.implicit_app(c, w.tuple({l, r})); // MIM_INFIX_SUGAR
    }
}

const IdPtrn* MatchExpr::Arm::ctor() const {
    auto id = ptrn()->isa<IdPtrn>();
    return id && !id->type() ? id : nullptr;
}

Lam* MatchExpr::Arm::emit(Emitter& e, const Def* dom) const {
    auto _      = e.world().push(loc());
    auto binder = dom && payload() ? payload() : ptrn();
    auto dom_t  = binder->emit_type(e);
    if (dom && !Checker::alpha<Checker::Check>(dom_t, dom))
        e.error()
            .e(binder->loc(), "pattern of type `{}` does not match the constructor's payload `{}`", dom_t, dom)
            .bail();
    auto pi  = e.world().pi(dom ? dom : dom_t, e.world().mut_hole_type());
    auto lam = e.world().mut_lam(pi);
    binder->emit_value(e, lam->var());
    return lam->set(true, body()->emit(e));
}

const Def* MatchExpr::emit_variant(Emitter& e, const Def* scrutinee, const Variant* variant) const {
    auto sel = DefVec(variant->num_ops());
    for (auto arm : arms()) {
        auto id = arm->ctor();
        if (!id)
            e.error()
                .e(arm->ptrn()->loc(), "an arm of a match on variant `{}` must name a constructor", variant)
                .bail();
        auto i = e.ctor(variant, id->dbg());
        if (sel[i]) {
            e.error().w(arm->loc(), "this arm is unreachable: constructor `{}` is already handled", id->dbg());
            continue;
        }
        sel[i] = arm->emit(e, variant->op(i));
    }

    auto missing = std::vector<std::string>();
    for (size_t i = 0, n = sel.size(); i != n; ++i)
        if (!sel[i]) missing.emplace_back(e.ctor_names(variant, i));
    if (!missing.empty()) e.error().e(loc(), "match expression has no arm for {}", fe::Join(missing, ", ")).bail();

    auto ops = DefVec{scrutinee};
    ops.append_range(sel);
    return e.world().match(ops);
}

const Def* MatchExpr::emit_(Emitter& e) const {
    DefVec ops;
    ops.emplace_back(scrutinee()->emit(e));
    if (auto variant = ops.front()->isa_type<Variant>()) return emit_variant(e, ops.front(), variant);

    for (auto arm : arms()) {
        if (arm->payload())
            e.error()
                .e(arm->loc(), "a constructor pattern needs a variant scrutinee")
                .n("but the scrutinee has type `{}`", type_of(ops.front()))
                .bail();
        ops.emplace_back(arm->emit(e));
    }
    auto res = e.world().match(ops);

    // Only a *source* arm is unreachable by mistake; substitution legitimately kills arms of a polymorphic match.
    auto cases = Match::cases(ops.front());
    for (size_t i = 0, n = num_arms(); i != n; ++i)
        if (std::ranges::none_of(cases, [&](const Def* c) { return Match::accepts(ops[i + 1], c); }))
            e.error().w(arm(i)->loc(), "this arm is unreachable: no case of `{}` matches `{}`", type_of(ops.front()),
                        ops[i + 1]->isa_type<Pi>()->dom());

    return res;
}

void PiExpr::Dom::emit_type(Emitter& e) const {
    // Created before the push: the Pi belongs to the whole function type, not just to this Dom.
    pi_        = decl_ ? decl_ : e.world().mut_pi(e.world().type_infer_univ(), is_implicit());
    auto _     = e.world().push(loc());
    auto dom_t = ptrn()->emit_type(e);

    if (ret()) {
        auto sigma = e.world().mut_sigma(2);
        auto var   = sigma->var();
        sigma->set(0, dom_t);
        ptrn()->emit_proj(e, var, 2, 0);
        auto ret_t = e.world().cn(ret()->emit_type(e));
        sigma->set(1, ret_t);

        if (auto imm = sigma->immutabilize())
            dom_t = imm;
        else
            dom_t = sigma;
        pi_->set_dom(dom_t);
    } else {
        pi_->set_dom(dom_t);
        ptrn()->emit_value(e, pi_->var());
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
    auto c = callee()->emit(e);
    if (auto cn = Pi::has_ret_pi(c->type())) {
        auto con  = e.world().mut_lam(cn);
        auto pair = e.world().tuple({arg()->emit(e), con});
        auto app  = e.world().app(c, pair);
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
    e.add_ctors(variant, this);
    return variant;
}

const Def* VariantExpr::emit_decl_(Emitter& e, const Def* type) const {
    return e.world().mut_variant(type, num_ctors());
}

void VariantExpr::emit_body_(Emitter& e, const Def* decl) const {
    auto variant = decl->as_mut<Variant>();
    variant->set(emit_payloads(e));
    e.add_ctors(variant, this);
}

const Def* TupleExpr::emit_(Emitter& e) const {
    DefVec elems(num_elems(), [&](size_t i) { return elem(i)->emit(e); });
    return e.world().tuple(elems);
}

const Def* SeqExpr::emit_(Emitter& e) const {
    auto s = arity()->emit_type(e);
    if (auto lit_s = Lit::isa(s); lit_s && *lit_s == 0) return e.world().unit(is_pack());

    if (arity()->dbg().is_anon()) { // immutable
        auto b = body()->emit(e);
        return e.world().seq(is_pack(), s, b);
    }

    auto t = e.world().type_infer_univ();
    auto a = e.world().mut_arr(t);
    a->set_shape(s);

    if (is_pack()) {
        auto p = e.world().mut_pack(a);
        p->set_shape(s);
        auto var = p->var();
        arity()->emit_value(e, var);
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
        arity()->emit_value(e, var);
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
    if (!annex_) return; // Skip emit if binding failed
    auto _      = e.world().push(loc());
    mim_type_   = type()->emit(e);
    auto& id    = annex_->id;
    auto plugin = annex_->plugin_id();

    std::tie(id.curry, id.trip) = Axm::infer_curry_and_trip(mim_type_);
    if (curry_) {
        if (curry_.lit_u() > id.curry)
            e.error().e(curry_.loc(), "curry counter cannot be greater than {}", id.curry).bail();
        else
            id.curry = curry_.lit_u();
    }

    if (trip_) {
        if (trip_.lit_u() > id.curry)
            e.error().e(trip_.loc(), "trip counter cannot be greater than curry counter {}", (int)id.curry).bail();
        else
            id.trip = trip_.lit_u();
    }

    auto norm = e.driver().normalizer(plugin, id.tag, sub_);
    auto name = annex_->qualified(e.driver(), dbg().sym());
    auto axm  = e.world().axm(norm, id.curry, id.trip, mim_type_, plugin, id.tag, sub_)->set(name);
    def_      = axm;
    e.world().annexes().attach(plugin, id.tag, sub_, name, axm);
}

void AxmDecl::Sibling::emit(Emitter& e) const {
    if (!annex_) return; // skip emit if binding failed
    auto& id    = annex_->id;
    auto plugin = annex_->plugin_id();
    auto norm   = e.driver().normalizer(plugin, id.tag, sub_);
    auto name   = annex_->qualified(e.driver(), dbg().sym());
    auto axm    = e.world().axm(norm, id.curry, id.trip, owner()->mim_type(), plugin, id.tag, sub_)->set(name);
    def_        = axm;
    e.world().annexes().attach(plugin, id.tag, sub_, name, axm);
}

void AliasDecl::emit(Emitter& e) const {
    if (!annex_) return; // skip emit if binding failed
    auto target = path()->decl();
    def_        = target->def();
    auto name   = annex_->qualified(e.driver(), dbg().sym());
    e.world().annexes().attach_alias(annex_->plugin_id(), annex_->id.tag, sub_, name);
}

void ModDecl::emit_decls(Emitter& e) const {
    for (auto decl : decls())
        decl->emit(e);
}

void ModDecl::emit(Emitter& e) const { emit_decls(e); }

void LetDecl::emit(Emitter& e) const {
    auto _ = e.world().push(loc());
    auto v = value()->emit(e);
    def_   = ptrn()->emit_value(e, v);
    if (auto id = ptrn()->isa<IdPtrn>()) e.attach(id->annex_, id->sub_, id->dbg().sym(), def_);
}

void RecDecl::emit(Emitter& e) const {
    for (auto curr = this; curr; curr = curr->next())
        curr->emit_decl(e);
    for (auto curr = this; curr; curr = curr->next())
        curr->emit_body(e);
}

void RecDecl::emit_decl(Emitter& e) const {
    auto _ = e.world().push(loc());
    def_   = body()->emit_decl(e, e.world().type_infer_univ());
    def_->set(dbg().sym());
}

void RecDecl::emit_body(Emitter& e) const {
    auto _ = e.world().push(loc());
    body()->emit_body(e, def_);
    // TODO immutabilize?
    e.attach(annex_, sub_, dbg().sym(), def_);
}

Lam* LamDecl::Dom::emit_value(Emitter& e) const {
    // Created before the push: the Lam belongs to the whole declaration, not just to this Dom.
    lam_     = e.world().mut_lam(pi_);
    auto _   = e.world().push(loc());
    auto var = lam_->var();

    if (ret()) {
        ptrn()->emit_proj(e, var, 2, 0);
        ret()->emit_proj(e, var, 2, 1);
    } else {
        ptrn()->emit_value(e, var);
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
        doms().back()->lam_->set_body(body()->emit(e));
    }

    // rewrite holes
    for (size_t i = 0, n = num_doms(); i != n; ++i) {
        auto rw  = VarRewriter(e.world());
        auto lam = dom(i)->lam_;
        auto pi  = lam->type()->as_mut<Pi>();
        for (auto dom : doms() | std::views::drop(i)) {
            if (auto var = pi->has_var()) rw.add(dom->lam_->var()->as<Var>(), var);
            auto cod = pi->codom();
            if (!cod || !cod->isa_mut<Pi>()) break;
            pi = cod->as_mut<Pi>();
        }

        if (auto cod = pi->codom(); cod && cod->has_dep(Dep::Hole)) pi->set(pi->dom(), rw.rewrite(cod));
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

void RuleDecl::emit(Emitter& e) const {
    auto _      = e.world().push(loc());
    auto meta_t = e.world().reform(var()->emit_type(e));
    auto rule   = e.world().mut_rule(meta_t)->set(dbg());
    var()->emit_value(e, rule->var());
    auto l = lhs()->emit(e);
    auto r = rhs()->emit(e);
    auto g = guard()->emit(e);
    rule->set(l, r, g);
    def_ = rule;
}

} // namespace mim::ast
