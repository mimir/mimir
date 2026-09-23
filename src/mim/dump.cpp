#include <fstream>
#include <ostream>
#include <ranges>

#include <fe/assert.h>

#include "mim/driver.h"
#include "mim/nest.h"
#include "mim/schedule.h"

#include "mim/ast/lexer.h"
#include "mim/ast/tok.h"

using namespace std::literals;

// During dumping, we classify Defs according to the following logic:
// * Inline: These Defs are *always* displayed with all of its operands "inline".
//   E.g.: (1, 2, 3).
// * All other Defs are referenced by its name/unique_name (see id) when they appear as an operand.
// * Mutables are either classifed as "decl" (see isa_decl).
//   In this case, recursing through the Defs' operands stops and this particular Decl is dumped as its own thing.
// * Or - if they are not a "decl" - they are basicallally handled like immutables.

namespace mim {

namespace {

Def* isa_decl(const Def* def) {
    if (auto mut = def->isa_mut()) {
        if (mut->isa<Hole>()) return nullptr; // a Hole stands for what it was set to, or for `?`
        if (mut->is_external() || mut->isa<Lam>() || (mut->sym() && mut->sym() != '_')) return mut;
    }
    return nullptr;
}

/// What the running dump decided about the Def%s it emits; `nullptr` where a diagnostic formats a bare Def.
struct Ctx {
    /// Names the dump binds itself: a `fun` calls its `ret` continuation `return`, whatever its Def::sym.
    /// Keyed by Var and index, because the projection itself need not exist as a Def while the World is frozen.
    DefMap<std::string> names;
    DefMap<nat_t> ret_projs; ///< Var -> index of the `ret` component within it.
    DefSet opaque;           ///< Def%s no pattern took apart, so their components have no name of their own.
    DefSet destructured;     ///< Def%s a pattern took apart, so their components print by name.
    DefSet whole_vars;       ///< Var%s used as a whole, which a `fun` - binding only its parameters - cannot name.
    DefSet arms;             ///< Lam%s a `match` spells out as its arms instead of declaring them.
    DefSet header;           ///< Def%s a Lam's header prints inline, as it precedes the block that `let`s them.
    bool in_header = false;
    DefSet inlined; ///< Def%s inside a binder the dump prints *inline* - they have no block to `let` them.
    /// A Lam's codomain refers to its Pi's Var - or its dependent domain's - but only the Lam's Var is bound by name.
    DefMap<const Def*> aliases;
    /// Distinct identifiers picked instead of Def::unique_name, so a re-read dump keeps them as its Def::sym%s.
    absl::flat_hash_set<std::string> taken;
    absl::flat_hash_map<std::string, size_t> counters; ///< Next numbered variant to try per base name.
    bool plain = false;
    std::string self; ///< `<world name>.`: the dumped file's own annexes are spelled without it.
    std::string mod;  ///< `<mod>.` while declaring the Axm%s of that `mod`, which does not see its own name.
};

std::string pick_name(Ctx* ctx, const Def* def) {
    auto sym  = def->sym();
    auto base = sym && sym != '_' && ast::Lexer::is_id(sym.view()) ? sym.str() : "_"s;
    auto name = base;
    // A numbered variant of an identifier is an identifier and never a keyword.
    if (base == "_" || def->world().driver().keys().find(sym) || ctx->taken.contains(name)) {
        auto sep = base == "_" ? "" : "_";
        auto& i  = ctx->counters[base];
        do
            name = std::format("{}{}{}", base, sep, ++i);
        while (ctx->taken.contains(name));
    }
    ctx->taken.emplace(name);
    return ctx->names.emplace(def, name).first->second;
}

/// The projection of the aliased Var that @p def - a projection of a Pi's Var - stands for; see Ctx::aliases.
const Def* alias_proj(Ctx* ctx, const Def* def) {
    if (auto ex = def->isa<Extract>())
        if (auto i = ctx->aliases.find(ex->tuple()); i != ctx->aliases.end() && !ctx->opaque.contains(i->second))
            if (auto l = Lit::isa(ex->index())) return i->second->proj(ex->tuple()->num_tprojs(), *l);
    return nullptr;
}

/// Def::unique_name - or a plain name while a World is dumped or a diagnostic is being formatted, where a gid is noise.
/// @p ctx is `nullptr` outside a running dump: there are no dump-bound names to consult then.
std::string name(Ctx* ctx, const Def* def) {
    if (ctx) {
        if (auto i = ctx->names.find(def); i != ctx->names.end()) return i->second;
        if (auto i = ctx->aliases.find(def); i != ctx->aliases.end()) return name(ctx, i->second);
        if (auto proj = alias_proj(ctx, def)) {
            auto res = name(ctx, proj);
            return ctx->names.emplace(def, std::move(res)).first->second;
        }
        if (auto ex = def->isa<Extract>())
            if (auto i = ctx->ret_projs.find(ex->tuple()); i != ctx->ret_projs.end())
                if (Lit::isa(ex->index()) == i->second) return "return"s;
        if (ctx->plain) return pick_name(ctx, def);
    }
    if (auto sym = def->sym(); sym && sym != '_' && PlainNames::claim(def->world().driver(), sym, def->gid()))
        return sym.str();
    return def->unique_name();
}

std::string id(Ctx* ctx, const Def* def) {
    if (def->is_external() || (!def->is_set() && def->isa<Lam>())) return def->sym().str();
    return name(ctx, def);
}

std::string_view external(const Def* def) {
    if (def->is_external()) return "extern "sv;
    return ""sv;
}

using Dump = Def::Dump;
using ast::Assoc;
using ast::Prec;
using ast::prec_assoc;

Prec def2prec(const Def* def) {
    // A Var's projection prints as its name and hence is atomic; see the Extract case in operator<<.
    if (auto ex = def->isa<Extract>())
        return ex->tuple()->isa<Var>() && ex->index()->isa<Lit>() ? Prec::Lit : Prec::Extract;
    if (def->isa<Insert>()) return Prec::Ins;
    if (def->isa<Join>()) return Prec::Union;
    if (auto inj = def->isa<Inj>()) {
        if (auto variant = inj->type()->isa<Variant>())
            return variant->op(inj->index()) == def->world().sigma() ? Prec::Extract : Prec::App;
        return Prec::Inj;
    }
    if (def->isa<Variant>()) return Prec::Lit; // always parenthesized
    if (def->isa<Reform>()) return Prec::App;
    // `Cn e` parses its domain at Prec::Bot, so it swallows whatever follows and only ever fits a closed context.
    if (auto pi = def->isa<Pi>()) return Pi::isa_cn(pi) ? Prec::Bot : Prec::Arrow;
    if (def->isa<Lam>() || def->isa<Match>())
        return Prec::Bot; // a λ- or match-expression swallows what follows it, too
    if (auto app = def->isa<App>()) {
        if (auto size = Idx::isa(app)) {
            if (auto l = Lit::isa(size)) {
                // clang-format off
                switch (*l) {
                    case 0x0'0000'0002_n:
                    case 0x0'0000'0100_n:
                    case 0x0'0001'0000_n:
                    case 0x1'0000'0000_n:
                    case             0_n: return Prec::Lit;
                    default: break;
                }
                // clang-format on
            }
        }
        return Prec::App;
    }

    return Prec::Lit;
}

/// This is a wrapper to dump a Def.
class Op {
public:
    Op(Ctx* ctx, const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : ctx_(ctx)
        , def_(def)
        , prec_(prec)
        , is_left_(is_left) {}

    /// @name Sub-expressions
    /// They inherit this Op's Ctx - which is how the running dump reaches the whole recursion.
    ///@{
    Op op(const Def* def, Prec prec = Prec::Bot, bool is_left = false) const { return {ctx_, def, prec, is_left}; }
    Op l(const Def* def, Prec prec = Prec::Bot) const { return {ctx_, def, prec, true}; }
    Op r(const Def* def, Prec prec = Prec::Bot) const { return {ctx_, def, prec, false}; }
    ///@}

    static auto map(Ctx* ctx, const auto& range, const char* sep = ", ", Prec prec = Prec::Bot) {
        return fe::Join(range | std::views::transform([ctx, prec](auto op) { return Op(ctx, op, prec); }), sep);
    }

    /// @name Getters
    ///@{
    Ctx* ctx() const { return ctx_; }
    Prec prec() const { return prec_; }
    bool is_left() const { return is_left_; }
    const Def* def() const { return def_; }
    const Def* operator->() const { return def_; }
    const Def* operator*() const { return def_; }
    explicit operator bool() const { return def_ != nullptr; }
    ///@}

private:
    Ctx* ctx_;
    const Def* def_;
    Prec prec_;
    bool is_left_;

    /// This will stream @p def as an operand.
    /// This is usually `id(def)` unless it can be displayed Inline.
    friend std::ostream& operator<<(std::ostream&, Op);
};

/// This is a wrapper to dump a Def "inline" and print it with all of its operands.
class Full : public Op {
public:
    Full(Ctx* ctx, const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : Op(ctx, def, prec, is_left) {}
    Full(Op op)
        : Full(op.ctx(), op.def(), op.prec(), op.is_left()) {}

    explicit operator bool() const { return is_inline(); }

    bool is_inline() const {
        if (auto ctx = this->ctx()) {
            if (ctx->inlined.contains(def())) return true;
            if (ctx->in_header && ctx->header.contains(def())) return true;
        }
        if (auto mut = def()->isa_mut()) {
            if (isa_decl(mut)) return false;
            return true;
        }

        if (def()->is_closed()) return true;

        if (auto app = def()->isa<App>()) {
            if (app->type()->isa<Pi>()) return true; // curried apps are printed inline
            if (app->type()->isa<Type>()) return true;
            if (app->callee()->isa<Axm>()) return app->callee_type()->num_doms() <= 1;
            return false;
        }

        return true;
    }

    bool needs_parens() const {
        if (!is_inline()) return false;

        auto child_prec = def2prec(def());
        if (child_prec < prec()) return true;
        if (child_prec > prec()) return false;

        switch (prec_assoc(prec())) {
            case Assoc::R: return is_left();
            case Assoc::L: return !is_left();
            case Assoc::N: return false;
        }
        fe::unreachable();
    }

    friend std::ostream& operator<<(std::ostream&, Full);
};

} // namespace
} // namespace mim

#ifndef DOXYGEN // clang-format off
template<> struct std::formatter<mim::Op  > : fe::ostream_formatter {};
template<> struct std::formatter<mim::Full> : fe::ostream_formatter {};
#endif // clang-format on

namespace mim {
namespace {

/// Def::num_tprojs, except that a *fused* Seq stays one binder: dumping freezes the World and its sub-Seqs
/// need not exist as Def%s.
nat_t num_binders(const Def* type) {
    if (auto seq = type->isa<Seq>(); seq && seq->shape().is_fused()) return 1;
    return type->num_tprojs();
}

/// A Seq's shape the way the surface syntax spells it: one axis per dimension, `2, 3` for a fused `(2, 3)`.
/// Falls back to the shape as a whole - also a legal spelling - if an axis does not exist as a Def.
std::string shape(Ctx* ctx, const Seq* seq) {
    auto s = seq->shape();
    auto r = s.rank();
    if (!r || *r <= 1) return std::format("{}", Op(ctx, *s));

    auto axes = s->projs(*r);
    if (std::ranges::any_of(axes, [](auto a) { return !a; })) return std::format("{}", Op(ctx, *s));
    return std::format("{}", Op::map(ctx, axes));
}

/// As above but each axis binds its index - `i: 2, j: 3`; a fused Seq binds one Var per axis.
std::string shape(Ctx* ctx, const Seq* seq, const Def* var) {
    auto s = seq->shape();
    auto r = s.rank();
    if (!r || *r <= 1) return std::format("{}: {}", Op(ctx, var), Op(ctx, *s));

    auto res = std::string();
    for (auto sep = ""; auto i : std::views::iota(nat_t(0), *r)) {
        auto axis   = var->proj(*r, i);
        auto extent = s[i];
        if (!extent) return std::format("{}: {}", Op(ctx, var), Op(ctx, *s));
        // A projection the body never mentions does not exist as a Def and hence has no name of its own.
        res += std::format("{}{}: {}", sep, axis ? std::format("{}", Op(ctx, axis)) : "_"s, Op(ctx, extent));
        sep = ", ";
    }
    return res;
}

/// May @p def of @p type be taken apart into @p n components?
/// A component the frozen World does not hand out has no name, so it has to fall back to @p type - and that
/// spells a *dependent* component with the domain's binders instead of this pattern's. That is where
/// destructuring stops: @p def stays opaque and its components print as `def#i` rather than by a dangling name.
bool destructible(Ctx* ctx, const Def* def, const Def* type, size_t n) {
    if (!def || n <= 1) return false;
    auto var = type->isa_mut<Sigma>() ? type->has_var() : nullptr;

    for (size_t i = 0; i != n; ++i) {
        if (def->proj(n, i)) continue; // it exists and hence brings its own name and type
        auto t = type->proj(n, i);
        if (!t || (var && t->has_free_var(var))) {
            if (ctx) ctx->opaque.emplace(def);
            return false;
        }
    }
    return true;
}

/// @p brckt selects the `[]` sub-patterns of a type over the `()` sub-patterns of a Lam's parameter list.
void ptrn(std::ostream& os, Ctx* ctx, const Def* def, const Def* type, bool brckt = false) {
    if (!def) return std::print(os, "_: {}", Op(ctx, type));

    auto n = def->num_tprojs();
    // Unless some component exists, nothing refers to one and the whole reads back the same.
    auto used = std::ranges::any_of(std::views::iota(size_t(0), n), [&](size_t i) { return def->proj(n, i); });
    if (!used || !destructible(ctx, def, type, n)) return std::print(os, "{}: {}", name(ctx, def), Op(ctx, type));
    if (ctx) {
        ctx->destructured.emplace(def);
        // Taken apart, a dependent Sigma no longer binds its Var itself; see Ctx::aliases.
        if (auto [_, var] = type->isa_binder<Sigma>(); var) ctx->aliases.emplace(var, def);
    }

    os << (brckt ? '[' : '(');
    for (auto sep = ""; auto i : std::views::iota(size_t(0), n)) {
        auto proj = def->proj(n, i);
        os << sep;
        // A projection's own type is the one where the binder has already been substituted for this Var.
        ptrn(os, ctx, proj, proj ? proj->type() : type->proj(n, i), brckt);
        sep = ", ";
    }
    // The alias keeps a name for the entity as a whole - which the body may well refer to.
    std::print(os, "{} as {}", brckt ? ']' : ')', name(ctx, def));
}

/// A Pi's domain the way the surface syntax binds it - a pattern, once the Var's projections are in use.
void dom_ptrn(std::ostream& os, Ctx* ctx, const Def* var, const Def* type, bool implicit) {
    auto l = implicit ? '{' : '[';
    auto r = implicit ? '}' : ']';
    auto n = var->num_tprojs();
    if (!destructible(ctx, var, type, n)) return std::print(os, "{}{}: {}{}", l, name(ctx, var), Op(ctx, type), r);

    os << l;
    for (auto sep = ""; auto i : std::views::iota(size_t(0), n)) {
        os << sep;
        auto proj = var->proj(n, i);
        ptrn(os, ctx, proj, proj ? proj->type() : type->proj(n, i), true);
        sep = ", ";
    }
    std::print(os, "{} as {}", r, name(ctx, var));
}

void bndr(std::ostream& os, Ctx* ctx, const Def* def, const Def* type) {
    if (def) return ptrn(os, ctx, def, def->type());
    std::print(os, "_: {}", Op(ctx, type));
}

/// @p num is how many binders the domain has; @p limit how many of them to print - a returning Lam hides its last.
void curry(std::ostream& os,
           Ctx* ctx,
           const Def* def,
           const Def* type,
           bool implicit,
           size_t num,
           size_t limit,
           bool alias) {
    auto l = implicit ? '{' : '(';
    auto r = implicit ? '}' : ')';

    if (limit == 0) return (void)(os << l << r);
    // Same as in ptrn: a parameter list only comes apart if every parameter is there to be named.
    if (limit == num && !destructible(ctx, def, type, num)) {
        if (def) return std::print(os, "{}{}: {}{}", l, name(ctx, def), Op(ctx, type), r);
        return std::print(os, "{}_: {}{}", l, Op(ctx, type), r);
    }

    os << l;
    for (auto sep = ""; auto i : std::views::iota(size_t(0), limit)) {
        os << sep;
        auto proj = def ? def->proj(num, i) : nullptr;
        bndr(os, ctx, proj, proj ? proj->type() : type->proj(num, i));
        sep = ", ";
    }
    os << r;
    if (alias && def) std::print(os, " as {}", name(ctx, def));
}

/// Is @p lam sugar for a `fun`?
/// That sugar folds the domain into `[<params>, Cn <ret>]`, so a *flat* domain with a trailing Cn stays a `con`.
bool isa_fun(const Lam* lam) {
    auto pi = lam->type();
    return Lam::isa_returning(lam) && num_binders(pi->dom()) == 2 && Pi::isa_basicblock(pi->dom(2, 1));
}

/// A Lam's parameter list, plus its filter where that is not the default for its position.
/// @p last marks a curried chain's final Lam: only that one hides a `fun`'s `ret` and defaults its filter to `ff`.
void lam_ptrn(std::ostream& os, Ctx* ctx, Lam* lam, bool fun, bool con, bool last, bool alias) {
    auto num   = num_binders(lam->type()->dom());
    auto limit = fun && last ? num - 1 : num;
    if (ctx) {
        if (auto var = lam->has_var()) {
            if (auto pi_var = lam->type()->has_var()) ctx->aliases.emplace(pi_var, var);
            // Kept whole, the domain prints as a Sigma that binds its Var itself.
            if (auto [_, sigma_var] = lam->dom()->isa_binder<Sigma>();
                sigma_var && (limit != num || destructible(ctx, var, lam->dom(), num)))
                ctx->aliases.emplace(sigma_var, var);
        }
    }
    curry(os, ctx, lam->has_var(), lam->type()->dom(), lam->type()->is_implicit(), num, limit, alias);
    auto& w = lam->world();
    if (auto dflt = last && (fun || con) ? w.lit_ff() : w.lit_tt(); lam->filter() != dflt)
        std::print(os, "@({})", Op(ctx, lam->filter()));
}

/// A Lam's result type; a `con` has none and a `fun` ascribes the domain of its `ret` continuation.
void lam_codom(std::ostream& os, Ctx* ctx, Lam* lam, bool fun, bool con) {
    if (fun)
        std::print(os, ": {}", Op(ctx, lam->ret_dom()));
    else if (!con)
        std::print(os, ": {}", Op(ctx, lam->type()->codom()));
}

/// The names of the constructors live in the frontend, so each case prints under its index.
std::string ctors(Ctx* ctx, const Variant* variant) {
    if (variant->num_ops() == 0) return "|";
    auto res = std::string();
    for (auto sep = ""; auto i : std::views::iota(size_t(0), variant->num_ops())) {
        res += std::format("{}| _{}", sep, i);
        if (auto op = variant->op(i); op != variant->world().sigma()) res += std::format(": {}", Op(ctx, op));
        sep = " ";
    }
    return res;
}

std::ostream& operator<<(std::ostream& os, Op op) {
    if (*op == nullptr) return os << "<nullptr>";
    if (auto d = Full(op)) return os << d;
    return os << id(op.ctx(), *op);
}

/// Streams @p d with all of its operands; Full::is_inline decides who gets here.
void full(std::ostream& os, Full d) {
    if (auto hole = d->isa_mut<Hole>()) {
        if (hole->is_set()) return std::print(os, "{}", d.op(hole->op()));
        return std::print(os, "?");
    }
    if (auto mut = d->isa_mut(); mut && !mut->is_set()) return std::print(os, "unset");
    if (d.needs_parens()) return std::print(os, "({})", Full(d.ctx(), *d));

    bool ascii = d->world().flags().ascii;
    auto arw   = ascii ? "->" : "→";
    auto al    = ascii ? "<<" : "«";
    auto ar    = ascii ? ">>" : "»";
    auto pl    = ascii ? "(<" : "‹";
    auto pr    = ascii ? ">)" : "›";
    auto bot   = ascii ? "bot" : "⊥";
    auto top   = ascii ? "top" : "⊤";

    if (auto type = d->isa<Type>()) {
        if (auto level = Lit::isa(type->level()); level && !ascii) {
            if (level == 0) return std::print(os, "*");
            if (level == 1) return std::print(os, "□");
        }
        return std::print(os, "Type {}", d.r(type->level(), Prec::App));
    } else if (auto reform = d->isa<Reform>()) {
        return std::print(os, "Rule {}", d.r(reform->dom(), Prec::App));
    } else if (d->isa<Univ>()) {
        return std::print(os, "Univ");
    } else if (d->isa<Nat>()) {
        return std::print(os, "Nat");
    } else if (d->isa<Idx>()) {
        return std::print(os, "Idx");
    } else if (auto ext = d->isa<Ext>()) {
        return std::print(os, "{}:{}", ext->isa<Bot>() ? bot : top, d.r(ext->type(), Prec::Lit));
    } else if (auto axm = d->isa<Axm>()) {
        auto axm_sym = axm->sym(); // a short Sym is stored inline, so its view must not outlive it
        auto sym     = axm_sym.view();
        if (auto ctx = d.ctx(); ctx && !ctx->self.empty() && sym.starts_with(ctx->self)) {
            sym.remove_prefix(ctx->self.size());
            if (!ctx->mod.empty() && sym.starts_with(ctx->mod)) sym.remove_prefix(ctx->mod.size());
        }
        return std::print(os, "{}", sym);
    } else if (auto lit = d->isa<Lit>()) {
        if (lit->type()->isa<Nat>()) {
            // clang-format off
            switch (lit->get()) {
                case 0x0'0000'0100_n: return std::print(os, "i8");
                case 0x0'0001'0000_n: return std::print(os, "i16");
                case 0x1'0000'0000_n: return std::print(os, "i32");
                default: return std::print(os, "{}", lit->get());
            }
            // clang-format on
        } else if (auto size = Idx::isa(lit->type())) {
            if (auto s = Lit::isa(size)) {
                // clang-format off
                switch (*s) {
                    case 0x0'0000'0002_n: return std::print(os, "{}", lit->get<bool>() ? "tt" : "ff");
                    case 0x0'0000'0100_n: return std::print(os, "{}I8" , lit->get());
                    case 0x0'0001'0000_n: return std::print(os, "{}I16", lit->get());
                    case 0x1'0000'0000_n: return std::print(os, "{}I32", lit->get());
                    case             0_n: return std::print(os, "{}I64", lit->get());
                    default: {
                        std::print(os, "{}", lit->get());
                        std::vector<uint8_t> digits;
                        for (auto z = *s; z; z /= 10) digits.emplace_back(z % 10);

                        // Raw UTF-8 units of a subscript - bytes to stream, not numbers to format.
                        if (ascii) {
                            os << '_';
                            for (auto d : digits | std::views::reverse)
                                os << char('0' + d);
                        } else {
                            for (auto d : digits | std::views::reverse)
                                os << uint8_t(0xE2) << uint8_t(0x82) << (uint8_t(0x80 + d));
                        }
                        return;
                    }
                }
                // clang-format on
            }
        }
        return std::print(os, "{}:{}", lit->get(), d.r(lit->type(), Prec::Lit));
    } else if (auto ex = d->isa<Extract>()) {
        // A Var's component prints by name - unless the pattern that would have bound that name kept the Var whole.
        auto opaque = d.ctx() && d.ctx()->opaque.contains(ex->tuple());
        // An aliased Var's component the Lam's Var does not hand out is spelled via the Lam's Var instead.
        if (d.ctx() && d.ctx()->aliases.contains(ex->tuple()) && !alias_proj(d.ctx(), ex)) opaque = true;
        auto bound = ex->tuple()->isa<Var>() || (d.ctx() && d.ctx()->destructured.contains(ex->tuple()));
        if (!opaque && bound && ex->index()->isa<Lit>()) return std::print(os, "{}", name(d.ctx(), ex));
        return std::print(os, "{}#{}", d.l(ex->tuple(), Prec::Extract), d.r(ex->index(), Prec::Extract));
    } else if (auto ins = d->isa<Insert>()) {
        auto tup = d.l(ins->tuple(), Prec::Extract);
        // `←` updates the whole `#`-path, so an Extract target needs parens to re-parse.
        if (auto ex = ins->tuple()->isa<Extract>(); ex && !(ex->tuple()->isa<Var>() && ex->index()->isa<Lit>()))
            std::print(os, "({})", tup);
        else
            std::print(os, "{}", tup);
        return std::print(os, "#{} ← {}", d.r(ins->index(), Prec::Extract), d.r(ins->value(), Prec::Ins));
    } else if (auto var = d->isa<Var>()) {
        return std::print(os, "{}", name(d.ctx(), var));
    } else if (auto [pi, var] = d->isa_binder<Pi>(); pi && pi->codom()->has_free_var(var)) {
        dom_ptrn(os, d.ctx(), var, pi->dom(), pi->is_implicit());
        return std::print(os, " {} {}", arw, d.r(pi->codom(), Prec::Arrow));
    } else if (auto pi = d->isa<Pi>()) {
        if (Pi::isa_cn(pi)) return std::print(os, "Cn {}", d.op(pi->dom()));
        if (pi->is_implicit())
            return std::print(os, "{{_: {}}} {} {}", d.op(pi->dom()), arw, d.r(pi->codom(), Prec::Arrow));
        return std::print(os, "{} {} {}", d.l(pi->dom(), Prec::Arrow), arw, d.r(pi->codom(), Prec::Arrow));
    } else if (auto lam = d->isa_mut<Lam>()) {
        // A Lam that has no declaration of its own is a λ-expression.
        auto whole = d.ctx() && lam->has_var() && d.ctx()->whole_vars.contains(lam->has_var());
        auto fun   = isa_fun(lam) && !whole;
        auto con   = Lam::isa_cn(lam) && !fun;
        std::print(os, "{}", fun ? "fn " : con ? "cn " : "λ ");
        lam_ptrn(os, d.ctx(), lam, fun, con, true, true);
        lam_codom(os, d.ctx(), lam, fun, con);
        // Like any tail, the body is spelled out - there is no block here that could `let`-bind it.
        if (isa_decl(lam->body())) return std::print(os, " = {}", d.op(lam->body()));
        return std::print(os, " = {}", Full(d.ctx(), lam->body()));
    } else if (auto app = d->isa<App>()) {
        if (auto size = Idx::isa(app)) {
            if (auto l = Lit::isa(size)) {
                // clang-format off
                switch (*l) {
                    case 0x0'0000'0002_n: return std::print(os, "Bool");
                    case 0x0'0000'0100_n: return std::print(os, "I8");
                    case 0x0'0001'0000_n: return std::print(os, "I16");
                    case 0x1'0000'0000_n: return std::print(os, "I32");
                    case             0_n: return std::print(os, "I64");
                    default: break;
                }
                // clang-format on
            }
        }

        if (Pi::isa_implicit(app->callee()->unfold_type())) {
            // An argument that is still a Hole is what the parser inserts anyway - and `?` has no spelling.
            if (app->arg()->has_dep(Dep::Hole)) return std::print(os, "{}", d.op(app->callee(), d.prec(), d.is_left()));
            // Spelling out an implicit argument needs `@`; juxtaposition would insert a Hole in front of it.
            return std::print(os, "{} @ {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
        }
        return std::print(os, "{} {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
    } else if (auto [sigma, var] = d->isa_binder<Sigma>(); sigma) {
        size_t i  = 0;
        auto elem = fe::StreamFn{[&](std::ostream& os) -> std::ostream& {
            auto sep = "";
            for (auto op : sigma->ops()) {
                os << sep;
                if (auto v = sigma->var(sigma->num_ops(), i++))
                    std::print(os, "{}: {}", d.op(v), d.op(op));
                else
                    os << d.op(op);
                sep = ", ";
            }
            return os;
        }};

        return std::print(os, "[{}]", elem);
    } else if (auto sigma = d->isa<Sigma>()) {
        return std::print(os, "[{}]", Op::map(d.ctx(), sigma->ops()));
    } else if (auto tuple = d->isa<Tuple>()) {
        return std::print(os, "({})", Op::map(d.ctx(), tuple->ops()));
    } else if (auto [arr, var] = d->isa_binder<Arr>(); arr) {
        return std::print(os, "{}{}; {}{}", al, shape(d.ctx(), arr, var), d.op(arr->body()), ar);
    } else if (auto arr = d->isa<Arr>()) {
        return std::print(os, "{}{}; {}{}", al, shape(d.ctx(), arr), d.op(arr->body()), ar);
    } else if (auto [pack, var] = d->isa_binder<Pack>(); pack) {
        return std::print(os, "{}{}; {}{}", pl, shape(d.ctx(), pack, var), d.op(pack->body()), pr);
    } else if (auto pack = d->isa<Pack>()) {
        return std::print(os, "{}{}; {}{}", pl, shape(d.ctx(), pack), d.op(pack->body()), pr);
    } else if (auto proxy = d->isa<Proxy>()) {
        return std::print(os, "(proxy#{} {})", proxy->tag(), Op::map(d.ctx(), proxy->ops()));
    } else if (auto join = d->isa<Join>()) {
        return std::print(os, "{}", Op::map(d.ctx(), join->ops(), " ∪ ", Prec::Union));
    } else if (auto variant = d->isa<Variant>()) {
        return std::print(os, "({})", ctors(d.ctx(), variant));
    } else if (auto inj = d->isa<Inj>()) {
        if (auto variant = inj->type()->isa<Variant>()) {
            auto ctor = std::format("{}#{}", d.l(variant, Prec::Extract), inj->index());
            if (variant->op(inj->index()) == d->world().sigma()) return std::print(os, "{}", ctor);
            return std::print(os, "{} {}", ctor, d.r(inj->value(), Prec::App));
        }
        return std::print(os, "{} inj {}", d.l(inj->value(), Prec::Inj), d.r(inj->type(), Prec::Inj));
    } else if (auto single = d->isa<Single>()) {
        return std::print(os, "{}{}{}", al, d.op(single->op()), ar);
    } else if (auto wrap = d->isa<Wrap>()) {
        return std::print(os, "{}{}{}", pl, d.op(wrap->op()), pr);
    } else if (auto match = d->isa<Match>();
               match && d.ctx() && match->num_arms() != 0 && d.ctx()->arms.contains(match->arm(0))) {
        std::print(os, "match {} with", d.op(match->scrutinee()));
        auto variant = match->scrutinee()->type()->isa<Variant>();
        for (size_t i = 0; auto arm : match->arms()) {
            auto lam = arm->as_mut<Lam>();
            os << " | ";
            // A variant's arms are its constructors, in order; a constructor without payload binds nothing.
            if (variant) std::print(os, "_{}", i++);
            if (!variant || lam->dom() != d->world().sigma()) {
                if (variant) os << ' ';
                ptrn(os, d.ctx(), lam->has_var(), lam->dom());
            }
            // Another `match` would swallow the arms that follow.
            std::print(os, " => {}", d.op(lam->body(), Prec::Where));
        }
        return;
    }

    // other
    auto tag = d->flags() == 0 ? std::string(d->node_name()) : std::format("{}#{}", d->node_name(), d->flags());
    if (d->ops().empty()) return std::print(os, "({})", tag);
    std::print(os, "({} {})", tag, Op::map(d.ctx(), d->ops(), " "));
}

std::ostream& operator<<(std::ostream& os, Full d) {
    full(os, d);
    return os;
}

/*
 * Dumper
 */

/// The Def%s the dump prints @p def with: a type nobody prints must not drag in a `let` that nothing refers to.
/// A Lam spells out its Pi, so only the components of that one are referenced.
template<class F>
void printed_deps(const Def* def, bool typed_let, F f) {
    if (auto lam = def->isa_mut<Lam>()) {
        f(lam->type()->dom());
        f(lam->type()->codom());
        for (auto op : lam->ops())
            f(op);
        return;
    }
    if (typed_let || def->isa<Lit>() || def->isa<Ext>() || def->isa<Inj>()) f(def->type());
    for (auto op : def->ops())
        f(op);
}

/// The Lam%s a curried declaration folds into one: `lam f (a) (b) = e`.
fe::Vector<Lam*> curry_chain(Lam* lam) {
    auto chain = fe::Vector<Lam*>();
    for (auto* curr = lam;;) {
        chain.emplace_back(curr);
        if (auto body = curr->body())
            if (auto next = body->isa_mut<Lam>(); next && !next->is_external()) {
                curr = next;
                continue;
            }
        break;
    }
    return chain;
}

/// Emits Def%s as Mim declarations.
///
/// Dump::Expr and Dump::Local walk Def::deps and nothing else - which is what makes them safe to call from a
/// debugger, where the World is usually half-built. Dump::Scope and Dump::All buy a better layout with a Nest:
/// it nests the mutables, and Scheduler::smart places everything else. Should that analysis choke on the very
/// program you wanted to look at, World::dot still shows it - its tooltips only ever use Dump::Expr.
class Dumper {
public:
    /// @p srcs are the files an `import` pulls in: they declare their own content, so a dump must not repeat it.
    Dumper(std::ostream& os, Dump mode, bool typed_let = false, absl::flat_hash_set<const fe::Src*> srcs = {})
        : os_(os)
        , mode_(mode)
        , typed_let_(typed_let)
        , srcs_(std::move(srcs)) {}

    /// Switches to plain names; @p reserved are names the dump refers to verbatim and hence must not pick.
    void plain(std::span<const std::string> reserved) {
        ctx_.plain = true;
        ctx_.taken.insert(reserved.begin(), reserved.end());
    }

    /// Declares the Axm%s of the dumped file itself as `axm`s, grouped into the `mod` their @p names nest them in.
    void dump_axms(std::string self, std::span<const std::pair<std::string, const Axm*>> axms) {
        ctx_.self = std::move(self);
        for (auto [_, axm] : axms)
            for (auto mut : axm->type()->local_muts())
                if (isa_decl(mut)) dump_muts(mut);

        sep(Prev::Decl);
        std::string_view curr_mod;
        for (auto [name, axm] : axms) {
            auto view = std::string_view(name);
            auto dot  = view.find('.');
            auto mod  = dot == std::string_view::npos ? std::string_view() : view.substr(0, dot);
            if (mod != curr_mod) {
                if (!curr_mod.empty()) std::println(os_, "{}}}", --tab_);
                if (!mod.empty()) std::println(os_, "{}mod {} {{", tab_, mod), ++tab_;
                curr_mod = mod;
                ctx_.mod = mod.empty() ? std::string() : std::string(mod) + ".";
            }
            std::print(os_, "{}axm {}: {}", tab_, mod.empty() ? view : view.substr(dot + 1), Op(&ctx_, axm->type()));
            auto [curry, trip] = Axm::infer_curry_and_trip(axm->type());
            if (axm->curry() != curry || axm->trip() != trip) std::print(os_, ", {}", axm->curry());
            if (axm->trip() != trip) std::print(os_, ", {}", axm->trip());
            std::println(os_, ";");
        }
        if (!curr_mod.empty()) std::println(os_, "{}}}", --tab_);
        ctx_.mod.clear();
        prev_ = Prev::Decl;
    }

    /// @name dump
    ///@{
    /// @p def and whatever Dumper::mode_ reaches from it.
    void dump(const Def* def) {
        if (auto mut = isa_decl(def)) return dump_muts(mut);

        if (mode_ != Dump::Local)
            for (auto mut : def->local_muts())
                if (isa_decl(mut)) dump_muts(mut);

        block_ = nullptr; // a non-decl opens no block, so nothing can be entered or placed into one
        emit_block(def);
        sep(Prev::Let);
        emit_tail(def, "");
    }

    ///@}

private:
    /// What the current block emitted last; only a declaration is worth setting apart.
    enum class Prev { None, Let, Decl };

    /// One scope: the Nest of a single closed mutable - which is the only thing a Scheduler can place into.
    void visit_scope(const Nest& nest) {
        auto root = nest.root()->mut();
        // An `import` re-declares what it *exports* - but a plain `let` in that file stays local to it, so the
        // dump has to spell that one out or nothing binds the name it refers to.
        if (root->is_external() && srcs_.contains(root->loc().src)) return;

        auto sched = Scheduler(nest);
        auto _     = fe::Restore(nest_, &nest);
        auto __    = fe::Restore(sched_, &sched);
        block_     = root;
        emit_block(root);
    }

    /// Schedules @p root into the current Dumper::block_ and emits whatever landed there.
    void emit_block(const Def* root) {
        schedule_(root, nullptr);
        bucket();
        emit(nullptr);
    }

    /// @name schedule
    ///@{
    /// The closed mutables @p mut reaches, callees first: Mim binds a name before its uses.
    void dump_muts(Def* mut) {
        auto todo = fe::Vector<Def*>();
        post_order(mut, todo);

        for (auto curr : todo) {
            if (mode_ == Dump::Local) {
                block_ = curr;
                emit_block(curr);
            } else {
                visit_scope(Nest(curr));
            }
        }
    }

    static bool is_spellable(const Match* match) {
        return std::ranges::all_of(match->arms(), [](const Def* arm) {
            auto lam = arm->isa_mut<Lam>();
            return lam && lam->is_set() && lam->filter() == lam->world().lit_tt();
        });
    }

    /// Like mim::post_order, but visits the mutables in the order the operands reach them.
    /// Def::local_muts orders by pointer, which a re-read dump need not reproduce.
    void post_order(Def* mut, fe::Vector<Def*>& todo) {
        if (!scheduled_.emplace(mut).second) return;

        if (mode_ == Dump::All || mode_ == Dump::Local) {
            auto muts = fe::Vector<Def*>();
            auto seen = DefSet();
            printed_deps(mut, typed_let_, [&](const Def* op) { local_muts(op, seen, muts); });
            for (auto local_mut : muts)
                post_order(local_mut, todo);
        }

        if ((mut->is_closed() && !ctx_.arms.contains(mut)) || mode_ == Dump::Local) todo.emplace_back(mut);
    }

    void local_muts(const Def* def, DefSet& seen, fe::Vector<Def*>& muts) {
        if (!def || def->local_muts().empty() || !seen.emplace(def).second) return;
        if (auto mut = def->isa_mut()) return (void)muts.emplace_back(mut);
        if (auto match = def->isa<Match>(); match && is_spellable(match))
            for (auto arm : match->arms())
                ctx_.arms.emplace(arm);
        printed_deps(def, typed_let_, [&](const Def* op) { local_muts(op, seen, muts); });
    }

    /// @name schedule
    ///@{
    void note_vars(const Def* def) {
        auto ex = def->isa<Extract>();
        for (auto op : def->ops())
            if (op && op->isa<Var>() && !(ex && op == ex->tuple() && ex->index()->isa<Lit>()))
                ctx_.whole_vars.emplace(op);
    }

    void schedule_(const Def* def, Def* curr) {
        if (!def) return;
        if (auto mut = isa_decl(def); mut && !ctx_.arms.contains(mut)) return schedule_mut(mut, curr);
        if (!done_.emplace(def).second) return;
        if (ctx_.arms.contains(def)) {
            // An arm has no block of its own either.
            auto lam = def->as_mut<Lam>();
            if (lam->body()->isa<Var>()) ctx_.whole_vars.emplace(lam->body());
            schedule_inline(lam->dom(), curr, ctx_.inlined);
            schedule_inline(lam->body(), curr, ctx_.inlined);
            return;
        }
        note_vars(def);
        printed_deps(def, typed_let_, [&](const Def* op) { schedule_(op, curr); });
        if (!Full(&ctx_, def)) order_.emplace_back(def, curr);
    }

    void schedule_mut(Def* mut, Def* curr) {
        if (open_.contains(mut)) recursive_.emplace(mut);
        if (!enter(mut)) return;
        if (!done_.emplace(mut).second) return;

        // The surface syntax has no block for a rule's parts, so whatever depends on its meta variable is inlined.
        if (auto rule = mut->isa_mut<Rule>()) {
            open_.emplace(mut);
            printed_deps(rule->type(), typed_let_, [&](const Def* op) { schedule_inline(op, mut, ctx_.inlined); });
            printed_deps(rule, typed_let_, [&](const Def* op) { schedule_inline(op, mut, ctx_.inlined); });
            open_.erase(mut);
            order_.emplace_back(mut, curr);
            return;
        }

        auto chain = mut->isa_mut<Lam>() ? curry_chain(mut->as_mut<Lam>()) : fe::Vector<Lam*>();
        open_.emplace(mut);

        if (chain.empty()) {
            for (auto op : mut->deps())
                schedule_(op, mut);
        } else {
            for (auto* lam : chain)
                if (lam != mut) done_.emplace(lam), absorbed_.emplace(lam, mut);
            // The chain spells out its Pi%s, so only their components are referenced.
            for (auto* lam : chain) {
                schedule_inline(lam->type()->dom(), mut, ctx_.header);
                schedule_inline(lam->filter(), mut, ctx_.header);
                if (lam == chain.back()) {
                    schedule_inline(lam->type()->codom(), mut, ctx_.header);
                    schedule_tail(lam->body(), mut);
                }
            }
        }

        open_.erase(mut);
        order_.emplace_back(mut, curr);
    }

    /// Schedules what @p def needs; @p def itself, noted in @p inline_set, prints inline.
    void schedule_inline(const Def* def, Def* curr, DefSet& inline_set) {
        if (!def || def->is_closed() || isa_decl(def)) return schedule_(def, curr);
        if (!inline_set.emplace(def).second) return;
        note_vars(def);
        printed_deps(def, typed_let_, [&](const Def* op) { schedule_inline(op, curr, inline_set); });
    }

    /// A Lam's body is the tail of its block, so it is emitted there instead of as a `let` of its own.
    void schedule_tail(const Def* def, Def* curr) {
        if (def && def->isa<Var>()) ctx_.whole_vars.emplace(def);
        if (!def || isa_decl(def)) return schedule_(def, curr);
        if (!done_.emplace(def).second) return;
        note_vars(def);
        printed_deps(def, typed_let_, [&](const Def* op) { schedule_(op, curr); });
    }

    /// Is @p mut ours to emit - or does it only get referenced by name?
    bool enter(Def* mut) const { return nest_ ? (*nest_)[mut] != nullptr : mut == block_; }

    /// Turns the schedule into one list per block: the Nest nests the mutables, Scheduler::smart places the rest.
    void bucket() {
        for (auto [def, curr] : order_) {
            auto mut = isa_decl(def);
            auto key = curr; // Dump::Local has one block per root and asks no analysis where anything belongs
            if (nest_) {
                // Nest::idom, not Nest::inest: a mutable only one sibling reaches belongs into *its* block.
                auto in = curr ? curr : block_;
                key     = mut ? owner((*nest_)[mut]->idom()) : owner(sched_->smart(in, def));
                // A binder that prints inline has no block of its own, so what belongs into it is inlined as well.
                if (key && inlines(key) && !(mut && recursive_.contains(mut))) {
                    ctx_.inlined.emplace(def);
                    continue;
                }
            }
            (key ? bucket_[key] : top_).emplace_back(def);
        }
        order_.clear();
    }

    /// Does @p mut print inline - be it one itself, or because it sits inside one that does?
    bool inlines(Def* mut) const {
        for (auto node = (*nest_)[mut]; node; node = node->inest())
            if (auto m = owner(node); m && (!isa_decl(m) || ctx_.arms.contains(m))) return true;
        return false;
    }

    /// The mutable whose block @p node stands for; `nullptr` for the top level.
    Def* owner(const Nest::Node* node) const {
        if (!node) return nullptr;
        auto mut = node->mut();
        if (!mut) return nullptr;                                                 // a *virtual* root is the top level
        if (auto i = absorbed_.find(mut); i != absorbed_.end()) return i->second; // a curried chain shares a block
        return mut;
    }
    ///@}

    /// @name emit
    ///@{
    void emit(Def* key) {
        auto defs = key ? std::move(bucket_[key]) : std::move(top_);
        for (auto def : defs) {
            auto mut = isa_decl(def);
            sep(mut ? Prev::Decl : Prev::Let);
            if (mut)
                emit_decl(mut);
            else
                emit_let(def);
        }
    }

    /// A declaration spans several lines, so a blank line sets it apart from its neighbors within the same block.
    void sep(Prev curr) {
        if (prev_ == Prev::Decl || (prev_ == Prev::Let && curr == Prev::Decl)) os_ << '\n';
        prev_ = curr;
    }

    void emit_let(const Def* def) {
        auto type = typed_let_ ? std::format(": {}", Op(&ctx_, def->type())) : std::string();
        std::println(os_, "{}let {}{} = {};", tab_, name(&ctx_, def), type, Full(&ctx_, def));
    }

    void emit_decl(Def* mut) {
        if (auto lam = mut->isa_mut<Lam>()) return emit_lam(lam);
        if (auto rule = mut->isa_mut<Rule>(); rule && rule->is_set()) return emit_rule(rule);
        if (!mut->is_set()) return emit_unset(mut);
        // `rec` binds the name for the body - which only a self-referential mutable needs; `extern` is out either way.
        auto kw = recursive_.contains(mut) ? "rec" : "let";
        // A `rec` only takes a bare variant.
        if (auto variant = mut->isa<Variant>())
            return std::println(os_, "{}{} {} = {};", tab_, kw, id(&ctx_, mut), ctors(&ctx_, variant));
        std::println(os_, "{}{} {} = {};", tab_, kw, id(&ctx_, mut), Full(&ctx_, mut));
    }

    void emit_rule(Rule* rule) {
        auto dom = rule->dom();
        auto num = num_binders(dom);
        std::print(os_, "{}rule {} ", tab_, id(&ctx_, rule));
        curry(os_, &ctx_, rule->has_var(), dom, false, num, num, false);
        std::print(os_, ": {}", Full(&ctx_, rule->lhs()));
        if (rule->guard() != rule->world().lit_tt()) std::print(os_, " when {}", Full(&ctx_, rule->guard()));
        std::println(os_, " => {};", Full(&ctx_, rule->rhs()));
    }

    /// Nothing declares a mutable that was never set, so leave a trace instead of an unreadable dump.
    void emit_unset(Def* mut) {
        std::println(os_, "{}// `{}: {}` is unset", tab_, id(&ctx_, mut), Op(&ctx_, mut->type()));
    }

    void emit_lam(Lam* lam) {
        auto chain = curry_chain(lam);
        auto last  = chain.back();
        auto whole = last->has_var() && ctx_.whole_vars.contains(last->has_var());
        auto fun   = isa_fun(last) && !shadows_ret(last) && !whole;
        auto con   = Lam::isa_cn(last) && !fun;

        // A `fun` binds its `ret` continuation as `return`, so that is the name its Var has to print with.
        if (fun) {
            if (auto var = last->has_var()) {
                if (auto num = num_binders(last->type()->dom()); num == 1)
                    ctx_.names.emplace(var, "return");
                else
                    ctx_.ret_projs.emplace(var, num - 1);
            }
        }
        // A Lam without a body forward-declares what a native translation unit provides - and only an `extern`
        // may go without one.
        if (!last->is_set()) return emit_bodyless(lam, chain, fun, con);

        // An inner Lam of the chain has no name; its outer Lam%s applied to their own Var%s reduce to it.
        auto ref = id(&ctx_, lam);
        for (size_t i = 1; i != chain.size(); ++i) {
            auto var = chain[i - 1]->has_var();
            if (!var) break;
            ref += std::format(" {}{}", chain[i - 1]->type()->is_implicit() ? "@" : "", name(&ctx_, var));
            ctx_.names.emplace(chain[i], std::format("({})", ref));
        }

        std::print(os_, "{}{}{} {}", tab_, external(lam), fun ? "fun" : con ? "con" : "lam", id(&ctx_, lam));
        {
            auto _ = fe::Restore(ctx_.in_header, true);
            for (auto* c : chain) {
                os_ << ' ';
                lam_ptrn(os_, &ctx_, c, fun, con, c == last, !fun || c != last);
            }
            lam_codom(os_, &ctx_, last, fun, con);
        }
        os_ << " =\n";

        ++tab_;
        rets_.emplace_back(fun ? last->has_var() : nullptr);
        auto _ = fe::Restore(prev_, Prev::None);
        emit(lam);
        sep(Prev::Let);
        emit_tail(last->body(), ";");
        rets_.pop_back();
        --tab_;
    }

    /// A Lam without a body only ever declares what some backend provides: `extern con f [mem.M 0, I32];`.
    void emit_bodyless(Lam* lam, const fe::Vector<Lam*>& chain, bool fun, bool con) {
        auto last = chain.back();
        auto dom  = last->type()->dom();
        std::print(os_, "{}extern {} {}", tab_, fun ? "fun" : con ? "con" : "lam", id(&ctx_, lam));
        auto num = num_binders(dom);
        if (fun) {
            os_ << '[';
            for (auto sep = ""; auto i : std::views::iota(size_t(0), num - 1)) {
                std::print(os_, "{}{}", sep, Op(&ctx_, dom->proj(num, i)));
                sep = ", ";
            }
            std::println(os_, "]: {};", Op(&ctx_, last->ret_dom()));
        } else {
            std::println(os_, "[{}]{};", Op::map(&ctx_, dom->projs(num)),
                         con ? std::string() : std::format(": {}", Op(&ctx_, last->type()->codom())));
        }
    }

    /// Tail position: what a block ends with is spelled out - it has no `let` of its own.
    void emit_tail(const Def* def, std::string_view end) {
        if (isa_decl(def))
            std::println(os_, "{}{}{}", tab_, Op(&ctx_, def), end);
        else
            std::println(os_, "{}{}{}", tab_, Full(&ctx_, def), end);
    }

    /// Would a `fun` here hide the `return` of an enclosing one @p lam might still refer to?
    /// The `return` is a projection of its Lam's Var, so capturing that Var at all is as precise as this gets.
    bool shadows_ret(Lam* lam) const {
        for (auto* var : rets_)
            if (var && lam->has_free_var(var)) return true;
        return false;
    }
    ///@}

    std::ostream& os_;
    Dump mode_;
    bool typed_let_;
    const Nest* nest_ = nullptr; ///< Nests the mutables of the scope being emitted.
    Scheduler* sched_ = nullptr; ///< Places everything else in it.
    Def* block_       = nullptr; ///< The mutable whose block we are filling; the only one in Dump::Local.
    Prev prev_        = Prev::None;
    fe::Tab tab_      = fe::Tab::spaces();
    fe::Vector<const Var*> rets_;
    fe::Vector<std::pair<const Def*, Def*>> order_; ///< What to emit, in dependency order, with its block's mutable.
    DefVec top_;
    MutMap<DefVec> bucket_;
    MutMap<Def*> absorbed_; ///< Inner Lam of a curried chain -> the chain's outermost one.
    DefSet done_;
    MutSet open_; ///< On the schedule stack - a Def reaching one of these is recursive.
    MutSet recursive_;
    Ctx ctx_;
    absl::flat_hash_set<const fe::Src*> srcs_;
    MutSet scheduled_;
};

} // namespace

/*
 * Def
 */

/// Def::Dump::Expr: one Def, one line, no analysis - and the backend of `std::formatter` for every Def pointer.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    auto _ = def->world().freeze();
    return os << Op(nullptr, def);
}

std::ostream& Def::stream(std::ostream& os, Dump mode) const {
    auto _ = world().freeze();
    if (mode == Dump::Expr) return os << this << std::endl;
    Dumper(os, mode, world().flags().mim_typed_let).dump(this);
    return os;
}

void Def::dump() const { std::cout << this << std::endl; }
void Def::dump(Dump mode) const { stream(std::cout, mode); }

void Def::write(Dump mode, const char* file) const {
    auto ofs = std::ofstream(file);
    stream(ofs, mode);
}

void Def::write(Dump mode) const {
    auto file = id(nullptr, this) + ".mim"s;
    write(mode, file.c_str());
}

/*
 * World
 */

void World::dump(std::ostream& os) {
    auto _       = freeze();
    auto old_gid = curr_gid();
    auto srcs    = absl::flat_hash_set<const fe::Src*>();
    for (const auto& import : driver().imports())
        srcs.emplace(import.src);
    for (const auto& import : driver().imports()) {
        auto kw = import.tag == ast::Tok::Tag::K_plugin ? "plugin" : "import";
        // The spelling was relative to the importing file; only the resolved path re-parses from here.
        // Generic format: a native Windows `\` would lex as an escape sequence inside the string literal.
        // A stem that is no identifier names no module, so the file has to be spliced instead.
        if (import.path)
            std::print(os, "{} \"{}\"{};\n", kw, ast::Lexer::escape(import.src->path().generic_string()),
                       ast::Lexer::is_id(import.src->path().stem().string()) ? "" : " as *");
        else
            std::print(os, "{} {};\n", kw, import.sym);
    }

    if (!driver().imports().entries().empty() && externals().size() != 0) os << '\n';

    // The local dump keeps every mutable to itself: no Nest that a broken program could trip over.
    auto reserved = std::vector<std::string>{"return"};
    for (const auto& import : driver().imports())
        reserved.emplace_back(fs::path(import.sym.view()).stem().string());
    for (auto mut : externals().muts())
        reserved.emplace_back(mut->sym().str());

    // An `import` declares its own annexes; those of the dumped file itself only exist if the dump declares them.
    auto self = (name() ? name() : sym("_default")).str() + ".";
    // Only a loaded plugin registers its annexes, so find them by what the externals reach.
    auto axms = std::vector<std::pair<std::string, const Axm*>>();
    auto seen = DefSet();
    auto todo = DefVec(externals().muts().begin(), externals().muts().end());
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if (!def || !seen.emplace(def).second) continue;
        if (auto axm = def->isa<Axm>(); axm && axm->sym().view().starts_with(self)) {
            auto name = axm->sym().str().substr(self.size());
            reserved.emplace_back(name.substr(0, name.find('.')));
            axms.emplace_back(std::move(name), axm);
        }
        for (auto dep : def->deps())
            todo.emplace_back(dep);
    }
    std::ranges::sort(axms, {}, [](const auto& p) { return p.second->flags(); });

    auto mode   = flags().mim_local ? Def::Dump::Local : Def::Dump::All;
    auto dumper = Dumper(os, mode, flags().mim_typed_let, std::move(srcs));
    dumper.plain(reserved);
    if (!axms.empty()) dumper.dump_axms(std::move(self), axms);
    for (auto mut : externals().muts())
        dumper.dump(mut);

    assertf(old_gid == curr_gid(), "new nodes created during dump. old_gid: {}; curr_gid: {}", old_gid, curr_gid());
}

void World::dump() { dump(std::cout); }

void World::debug_dump() {
    if (log().level() >= fe::Log::Level::Debug) dump(log().ostream());
}

void World::write(const char* file) {
    auto ofs = std::ofstream(file);
    dump(ofs);
}

void World::write() {
    auto file = (name() ? name() : sym("_default")).str() + ".mim"s;
    write(file.c_str());
}

} // namespace mim
