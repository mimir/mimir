#include "unparse.h"

#include <ranges>

#include "mim/axm.h"
#include "mim/check.h"
#include "mim/lattice.h"
#include "mim/rule.h"
#include "mim/union.h"
#include "mim/world.h"

#include "mim/ast/lexer.h"

#include "names.h"

namespace mim::dump {

using namespace ast;
using Tag  = Tok::Tag;
using Kind = Layout::Kind;

namespace {

/// The type `Idx size` is sugar for, if there is one.
std::optional<Tag> idx_type(nat_t size) {
    switch (size) {
        case 0x0'0000'0002_n: return Tag::K_Bool;
        case 0x0'0000'0100_n: return Tag::K_I8;
        case 0x0'0001'0000_n: return Tag::K_I16;
        case 0x1'0000'0000_n: return Tag::K_I32;
        case 0_n: return Tag::K_I64;
        default: return {};
    }
}

/// The name of a `Nat` that is the size of a machine type.
std::optional<Tag> nat_sugar(nat_t val) {
    switch (val) {
        case 0x0'0000'0100_n: return Tag::K_i8;
        case 0x0'0001'0000_n: return Tag::K_i16;
        case 0x1'0000'0000_n: return Tag::K_i32;
        default: return {};
    }
}

} // namespace

Unparser::Unparser(AST& ast, const Layout& layout)
    : ast_(ast)
    , layout_(layout)
    , world_(layout.world()) {}

/*
 * builders
 */

Ptr<Expr> Unparser::prim(Tag tag) { return mk<PrimaryExpr>(tag); }
Ptr<Expr> Unparser::raw(std::string_view text) { return mk<RawExpr>(ast_.sym(text)); }
Ptr<Expr> Unparser::app(Ptr<Expr> callee, Ptr<Expr> arg) { return mk<AppExpr>(callee, arg); }
Ptr<Expr> Unparser::parens(Ptr<Expr> expr) { return ast_.ptr<TupleExpr>(Loc(), Ptrs<Expr>{expr}); }
Ptr<IdPtrn> Unparser::id(Sym sym, Ptr<Expr> type) { return mk<IdPtrn>(Dbg(sym), type); }

Ptr<Expr> Unparser::infix(Ptr<Expr> lhs, Tag tag, Ptr<Expr> rhs) {
    return mk<InfixExpr>(lhs, Tok(Loc(), tag), rhs, Ptr<Expr>());
}

Ptr<TuplePtrn> Unparser::tuple_ptrn(Tag delim_l, Ptrs<Ptrn> ptrns) {
    return ast_.ptr<TuplePtrn>(Loc(), delim_l, ptrns);
}

Ptr<Expr> Unparser::path(Sym sym) {
    auto dbgs = Dbgs();
    for (auto part : sym.view() | std::views::split('.'))
        dbgs.emplace_back(Dbg(ast_.sym(std::string_view(part.begin(), part.end()))));
    return ast_.ptr<PathExpr>(ast_.ptr<Path>(Loc(), dbgs));
}

Sym Unparser::axm_sym(const Axm* axm) {
    auto sym         = axm->sym();
    auto view        = sym.view();
    const auto& self = layout_.opts().self;
    if (!self.empty() && view.starts_with(self)) {
        view.remove_prefix(self.size());
        if (!mod_.empty() && view.starts_with(mod_)) view.remove_prefix(mod_.size());
    }
    return ast_.sym(view);
}

/*
 * expressions
 */

Ptr<Expr> Unparser::expr(const Def* def, bool header) {
    if (!def) return raw("<nullptr>");
    def = solve(def);
    if (def->isa_mut<Hole>()) return mk<HoleExpr>();
    if (auto sym = layout_.name(def, ctx_); sym && !(header && layout_.in_header(def))) return path(sym);
    return full(def, header);
}

Ptr<Expr> Unparser::full(const Def* def, bool header) {
    auto mut = def->isa_mut();
    if (!mut) return full_(def, header);
    if (!mut->is_set()) return raw("unset");
    if (layout_.unspellable(mut) || !active_.emplace(mut).second) return raw(Names::fallback(mut));
    auto res = full_(def, header);
    active_.erase(mut);
    return res;
}

Ptr<Expr> Unparser::full_(const Def* def, bool header) {
    auto& w  = world_;
    auto sub = [&](const Def* d) { return expr(d, header); };

    if (auto type = def->isa<Type>()) {
        if (auto level = Lit::isa(type->level())) {
            if (*level == 0) return prim(Tag::T_star);
            if (*level == 1) return prim(Tag::T_box);
        }
        return mk<TypeExpr>(sub(type->level()));
    }
    if (def->isa<Univ>()) return prim(Tag::K_Univ);
    if (def->isa<Nat>()) return prim(Tag::K_Nat);
    if (def->isa<Idx>()) return prim(Tag::K_Idx);
    if (auto reform = def->isa<Reform>()) return mk<RuleExpr>(sub(reform->dom()));
    if (auto ext = def->isa<Ext>())
        return mk<LitExpr>(Tok(Loc(), ext->isa<Bot>() ? Tag::T_bot : Tag::T_top), sub(ext->type()));
    if (auto axm = def->isa<Axm>()) return path(axm_sym(axm));
    if (auto lit = def->isa<Lit>()) return literal(lit, header);
    if (def->isa<Var>()) return raw(Names::fallback(def));
    if (auto ex = def->isa<Extract>()) return infix(sub(ex->tuple()), Tag::T_extract, sub(ex->index()));
    if (auto ins = def->isa<Insert>()) {
        auto tup = sub(ins->tuple());
        // `←` updates the whole `#` path to its left, so a spelled-out Extract must not join it.
        if (auto ex = ins->tuple()->isa<Extract>(); ex && !layout_.name(ex)) tup = parens(tup);
        return infix(infix(tup, Tag::T_extract, sub(ins->index())), Tag::T_arrow_l, sub(ins->value()));
    }
    if (auto [pi, var] = def->isa_binder<Pi>(); pi && pi->codom()->has_free_var(var)) {
        auto b        = layout_.binder(pi);
        auto implicit = pi->is_implicit();
        auto dom      = mk<PiExpr::Dom>(ptrn(b->root, implicit ? Tag::D_brace_l : Tag::D_brckt_l, header));
        return mk<PiExpr>(implicit ? Tag::D_brace_l : Tag::Nil, dom, sub(pi->codom()));
    }
    if (auto pi = def->isa<Pi>()) {
        if (Pi::isa_cn(pi)) return mk<PiExpr>(Tag::K_Cn, mk<PiExpr::Dom>(id(Sym(), sub(pi->dom()))), Ptr<Expr>());
        if (pi->is_implicit()) {
            auto dom = mk<PiExpr::Dom>(tuple_ptrn(Tag::D_brace_l, {id(ast_.sym_anon(), sub(pi->dom()))}));
            return mk<PiExpr>(Tag::D_brace_l, dom, sub(pi->codom()));
        }
        return infix(sub(pi->dom()), Tag::T_arrow_r, sub(pi->codom()));
    }
    if (auto lam = def->isa_mut<Lam>()) return ast_.ptr<LamExpr>(lam_decl(lam, Dbg(), Mods{}, Ptr<RecDecl>()));
    if (auto app = def->isa<App>()) {
        if (auto size = Idx::isa(app))
            if (auto l = Lit::isa(size))
                if (auto tag = idx_type(*l)) return prim(*tag);
        if (Pi::isa_implicit(app->callee()->unfold_type())) {
            // An argument that is still a Hole is what the parser inserts anyway - and `?` has no spelling.
            if (app->arg()->has_dep(Dep::Hole)) return sub(app->callee());
            return infix(sub(app->callee()), Tag::T_at, sub(app->arg()));
        }
        return this->app(sub(app->callee()), sub(app->arg()));
    }
    if (auto sigma = def->isa<Sigma>()) {
        auto b  = sigma->has_var() ? layout_.binder(def->isa_mut()) : nullptr;
        auto ps = Ptrs<Ptrn>();
        for (size_t i = 0; auto op : sigma->ops()) {
            auto sym = b && i < b->root.comps.size() ? layout_.slot_name(b->root.comps[i]) : Sym();
            ps.emplace_back(id(sym, sub(op)));
            ++i;
        }
        return ast_.ptr<SigmaExpr>(tuple_ptrn(Tag::D_brckt_l, ps));
    }
    if (auto tuple = def->isa<Tuple>()) {
        auto es = Ptrs<Expr>();
        for (auto op : tuple->ops())
            es.emplace_back(sub(op));
        return ast_.ptr<TupleExpr>(Loc(), es);
    }
    if (auto seq = def->isa<Seq>()) return this->seq(seq, header);
    if (auto single = def->isa<Single>()) return mk<SingleExpr>(false, sub(single->op()));
    if (auto wrap = def->isa<Wrap>()) return mk<SingleExpr>(true, sub(wrap->op()));
    if (auto join = def->isa<Join>(); join && join->num_ops() != 0) {
        auto acc = Ptr<Expr>();
        for (auto op : join->ops())
            acc = acc ? infix(acc, Tag::T_union, sub(op)) : sub(op);
        return acc;
    }
    if (auto variant = def->isa<Variant>()) {
        auto ctors = Ptrs<VariantExpr::Ctor>();
        for (size_t i = 0; auto op : variant->ops()) {
            auto dbg = Dbg(ast_.sym(std::format("_{}", i++)));
            ctors.emplace_back(mk<VariantExpr::Ctor>(dbg, op == w.sigma() ? Ptr<Expr>() : sub(op)));
        }
        return ast_.ptr<VariantExpr>(Loc(), ctors);
    }
    if (auto inj = def->isa<Inj>()) {
        if (auto variant = inj->type()->isa<Variant>()) {
            auto index = mk<LitExpr>(Tok(Loc(), u64(inj->index())), Ptr<Expr>());
            auto ctor  = infix(sub(variant), Tag::T_extract, index);
            if (variant->op(inj->index()) == w.sigma()) return ctor;
            return this->app(ctor, sub(inj->value()));
        }
        return infix(sub(inj->value()), Tag::K_inj, sub(inj->type()));
    }
    if (auto match = def->isa<Match>(); match && layout_.spellable(match)) {
        auto variant = match->scrutinee()->type()->isa<Variant>();
        auto arms    = Ptrs<MatchExpr::Arm>();
        for (size_t i = 0; auto arm : match->arms()) {
            auto lam  = arm->as_mut<Lam>();
            auto b    = layout_.binder(lam);
            auto body = sub(lam->body());
            if (variant) {
                // A variant's arms are its cases, in order - selected by index, since names live in the frontend.
                auto payload = lam->dom() == w.sigma() ? Ptr<Ptrn>() : ptrn(b->root, Tag::D_paren_l, header);
                arms.emplace_back(
                    mk<MatchExpr::Arm>(std::optional<nat_t>(i), id(ast_.sym_anon(), Ptr<Expr>()), payload, body));
            } else {
                arms.emplace_back(mk<MatchExpr::Arm>(std::optional<nat_t>(), ptrn(b->root, Tag::D_paren_l, header),
                                                     Ptr<Ptrn>(), body));
            }
            ++i;
        }
        return ast_.ptr<MatchExpr>(Loc(), sub(match->scrutinee()), arms);
    }
    return fallback(def, header);
}

Ptr<Expr> Unparser::literal(const Lit* lit, bool header) {
    auto val = lit->get();
    if (lit->type()->isa<Nat>()) {
        if (auto tag = nat_sugar(val)) return prim(*tag);
        return mk<LitExpr>(Tok(Loc(), u64(val)), Ptr<Expr>());
    }
    if (auto size = Idx::isa(lit->type()))
        if (auto s = Lit::isa(size)) {
            if (*s == 2) return prim(val ? Tag::K_tt : Tag::K_ff);
            return mk<LitExpr>(Tok(Loc(), u64(*s), u64(val)), Ptr<Expr>());
        }
    return mk<LitExpr>(Tok(Loc(), u64(val)), expr(lit->type(), header));
}

/// One axis per dimension - `«i: 2, j: 3; T»` - as long as every axis exists as a Def; the shape as a whole otherwise.
Ptr<Expr> Unparser::seq(const Seq* seq, bool header) {
    auto is_pack = seq->isa<Pack>() != nullptr;
    auto shape   = seq->shape();
    auto r       = shape.rank();
    auto b       = seq->has_var() ? layout_.binder(seq->isa_mut()) : nullptr;
    auto body    = expr(seq->body(), header);
    auto axis    = [&](Sym sym, const Def* extent, Ptr<Expr> body) {
        return mk<SeqExpr>(is_pack, IdPtrn::make_id(ast_, Dbg(sym), expr(extent, header)), body);
    };

    if (r && *r > 1 && (!b || b->root.comps.size() == *r)) {
        auto axes = DefVec();
        for (auto k : std::views::iota(nat_t(0), *r))
            axes.emplace_back(shape[k]);
        if (std::ranges::none_of(axes, [](auto a) { return a == nullptr; })) {
            for (auto k : std::views::iota(nat_t(0), *r) | std::views::reverse)
                body = axis(b ? layout_.slot_name(b->root.comps[k]) : Sym(), axes[k], body);
            return body;
        }
    }
    return axis(b ? layout_.slot_name(b->root) : Sym(), *shape, body);
}

/// Text for what has no surface syntax; the operands still print like any others.
Ptr<Expr> Unparser::fallback(const Def* def, bool header) {
    auto tag = def->flags() == 0 ? std::string(def->node_name()) : std::format("{}#{}", def->node_name(), def->flags());
    auto res = raw(tag);
    for (auto op : def->ops())
        res = app(res, expr(op, header));
    return res;
}

/*
 * patterns
 */

Ptr<Ptrn> Unparser::leaf(const Slot& s, Tag delim_l, bool header) {
    auto sym = layout_.slot_name(s);
    if (!sym && delim_l != Tag::D_brckt_l) sym = ast_.sym_anon();
    return id(sym, s.type ? expr(s.type, header) : raw("?"));
}

Ptr<Ptrn> Unparser::ptrn(const Slot& s, Tag delim_l, bool header) {
    if (!s.pattern) {
        if (auto sigma = s.type ? s.type->isa<Sigma>() : nullptr; sigma && sigma->num_ops() == 0 && !s.used)
            return tuple_ptrn(delim_l, {}); // `()`
        return tuple_ptrn(delim_l, {leaf(s, delim_l, header)});
    }
    auto inner = delim_l == Tag::D_brckt_l ? Tag::D_brckt_l : Tag::D_paren_l;
    auto ps    = Ptrs<Ptrn>();
    for (auto& c : s.comps)
        ps.emplace_back(c.pattern ? ptrn(c, inner, header) : leaf(c, delim_l, header));
    auto res = Ptr<Ptrn>(tuple_ptrn(delim_l, ps));
    if (auto sym = s.used ? layout_.slot_name(s) : Sym()) res = mk<AliasPtrn>(res, Dbg(sym));
    return res;
}

/*
 * declarations
 */

Ptr<LamDecl> Unparser::lam_decl(Lam* head, Dbg dbg, Mods mods, Ptr<RecDecl> next) {
    auto chain    = layout_.chain(head);
    auto last     = chain.back();
    auto kind     = layout_.kind(last);
    auto fun      = kind == Kind::Fun;
    auto con      = kind == Kind::Con;
    auto decl     = bool(dbg);
    auto has_body = last->is_set();
    auto tag      = fun ? (decl ? Tag::K_fun : Tag::K_fn)
                  : con ? (decl ? Tag::K_con : Tag::K_cn)
                        : (decl ? Tag::K_lam : Tag::T_lm);

    auto doms = Ptrs<LamDecl::Dom>();
    auto _    = fe::Restore(ctx_, static_cast<const Lam*>(last));
    for (auto m : chain) {
        auto __      = fe::Restore(ctx_, static_cast<const Lam*>(m));
        auto& root   = layout_.binder(m)->root;
        auto is_last = m == last;
        auto& slot   = fun && is_last && root.comps.size() == 2 ? root.comps[0] : root;
        auto delim   = !has_body ? Tag::D_brckt_l : m->type()->is_implicit() ? Tag::D_brace_l : Tag::D_paren_l;
        auto filter  = Ptr<Expr>();
        if (has_body) {
            auto dflt = is_last && (fun || con) ? world_.lit_ff() : world_.lit_tt();
            if (m->filter() != dflt) {
                filter = expr(m->filter(), true);
                if (filter->prec() < Prec::Lit) filter = parens(filter);
            }
        }
        doms.emplace_back(mk<LamDecl::Dom>(ptrn(slot, delim, true), filter));
    }
    if (fun) doms.back()->add_ret(ast_, expr(last->dom()->op(1)->as<Pi>()->dom(), true));
    auto codom = !fun && !con ? expr(last->type()->codom(), true) : Ptr<Expr>();
    ctx_       = nullptr; // the body is no header
    auto body  = Ptr<Expr>();
    if (has_body) body = layout_.block(head) ? block(*layout_.block(head)) : expr(last->body());
    if (!has_body) mods.is_extern = true;
    return ast_.ptr<LamDecl>(Loc(), mods, tag, dbg, codom, body, next, doms);
}

Ptr<RecDecl> Unparser::rec_decl(Def* mut, Ptr<RecDecl> next) {
    auto dbg = Dbg(layout_.name(mut));
    if (auto lam = mut->isa_mut<Lam>()) return lam_decl(lam, dbg, Mods{.is_extern = lam->is_external()}, next);
    return mk<RecDecl>(Mods{}, dbg, full(mut), next);
}

Ptr<ValDecl> Unparser::rule_decl(Rule* rule) {
    auto b     = layout_.binder(rule);
    auto guard = rule->guard() == world_.lit_tt() ? prim(Tag::K_tt) : expr(rule->guard());
    return mk<RuleDecl>(Dbg(layout_.name(rule)), ptrn(b->root, Tag::D_paren_l, false), expr(rule->lhs()),
                        expr(rule->rhs()), guard, false);
}

/// `axm`s of one `mod`; consecutive ones of the same type share a declaration.
void Unparser::axms(const Item& item, Ptrs<ValDecl>& out) {
    auto prefix          = item.mod ? std::string(item.mod.view()) + "." : std::string();
    auto old             = std::exchange(mod_, prefix);
    auto ds              = Ptrs<ValDecl>();
    const AxmDecl* owner = nullptr;
    const Axm* owner_axm = nullptr;
    for (auto axm : item.axms) {
        auto sym  = axm->sym();
        auto name = sym.view().substr(layout_.opts().self.size());
        auto tag  = Dbg(ast_.sym(name.substr(prefix.size())));
        if (owner && owner_axm->type() == axm->type() && owner_axm->curry() == axm->curry()
            && owner_axm->trip() == axm->trip()) {
            ds.emplace_back(mk<AxmDecl::Sibling>(Vis::Pub, tag, owner));
            continue;
        }
        auto [curry, trip] = Axm::infer_curry_and_trip(axm->type());
        auto curry_tok     = axm->curry() != curry || axm->trip() != trip ? Tok(Loc(), u64(axm->curry())) : Tok();
        auto trip_tok      = axm->trip() != trip ? Tok(Loc(), u64(axm->trip())) : Tok();
        auto decl          = mk<AxmDecl>(Vis::Pub, tag, expr(axm->type()), Dbg(), curry_tok, trip_tok);
        owner              = decl.get();
        owner_axm          = axm;
        ds.emplace_back(decl);
    }
    mod_ = old;
    if (item.mod)
        out.emplace_back(mk<ModDecl>(Vis::Pub, Dbg(item.mod), ast_.scope(), ast_.copy(ds)));
    else
        out.append_range(ds);
}

Ptrs<ValDecl> Unparser::decls(const Block& b) {
    auto out = Ptrs<ValDecl>();
    for (auto& item : b.items) {
        switch (item.tag) {
            case Item::Tag::Let: {
                auto type = layout_.opts().typed_let ? expr(item.def->type()) : Ptr<Expr>();
                out.emplace_back(mk<LetDecl>(Mods{}, id(layout_.name(item.def), type), full(item.def)));
                break;
            }
            case Item::Tag::Axms: axms(item, out); break;
            case Item::Tag::Decl: {
                auto m = item.muts.front();
                if (item.muts.size() == 1 && !item.rec && !m->isa<Lam>() && !m->isa<Rule>()) {
                    // A mutable that neither recurs nor is a function is a plain `let` - `rec` takes only some bodies.
                    auto typed = !m->is_set() || layout_.opts().typed_let;
                    auto type  = typed ? expr(m->type()) : Ptr<Expr>();
                    out.emplace_back(mk<LetDecl>(Mods{}, id(layout_.name(m), type), full(m)));
                    break;
                }
                auto next = Ptr<RecDecl>();
                for (auto mut : item.muts | std::views::reverse) {
                    if (auto rule = mut->isa_mut<Rule>()) {
                        out.emplace_back(rule_decl(rule));
                        continue;
                    }
                    next = rec_decl(mut, next);
                }
                if (next) out.emplace_back(next);
                break;
            }
        }
    }
    return out;
}

/// Always a DeclExpr - even without `let`s - so that a declaration's body starts on a line of its own.
Ptr<Expr> Unparser::block(const Block& b) {
    auto ds = decls(b);
    return ast_.ptr<DeclExpr>(Loc(), expr(b.tail), false, ds);
}

Ptr<ValDecl> Unparser::use(const Driver::Imports::Entry& entry) {
    auto stem = entry.src->path().stem().string();
    auto path = ast_.ptr<Path>(Loc(), Dbgs{Dbg(entry.path ? ast_.sym(stem) : entry.sym)});
    // The spelling was relative to the importing file; only the resolved path re-parses from here.
    // Generic format: a native Windows `\` would lex as an escape sequence inside the string literal.
    auto file   = entry.path ? ast_.sym(entry.src->path().generic_string()) : Sym();
    auto splice = entry.path && !Lexer::is_id(stem); // a stem that is no identifier names no module
    return mk<UseDecl>(Mods{}, entry.tag, path, file, Dbg(), splice, nullptr);
}

} // namespace mim::dump
