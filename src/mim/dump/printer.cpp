#include "printer.h"

#include "mim/driver.h"

#include "mim/ast/lexer.h"

using namespace std::literals;

namespace mim::dump {

using ast::Assoc;
using ast::prec_assoc;

namespace {

/// The name the surface syntax has for `Idx size`.
std::optional<std::string_view> idx_name(nat_t size) {
    // clang-format off
    switch (size) {
        case 0x0'0000'0002_n: return "Bool";
        case 0x0'0000'0100_n: return "I8";
        case 0x0'0001'0000_n: return "I16";
        case 0x1'0000'0000_n: return "I32";
        case             0_n: return "I64";
        default:              return std::nullopt;
    }
    // clang-format on
}

/// The name of @p def if it is an `Idx size` the surface syntax has one for.
std::optional<std::string_view> isa_idx_name(const Def* def) {
    if (auto size = Idx::isa(def))
        if (auto l = Lit::isa(size)) return idx_name(*l);
    return std::nullopt;
}

/// A constructor without payload is spelled without an argument.
bool has_payload(const Variant* variant, size_t i) { return variant->op(i) != variant->world().sigma(); }

/// Does @p var occur in @p t only as projections, which a pattern binds by name?
bool only_projs(const Def* t, const Var* var, DefSet& done) {
    if (!t->has_free_var(var) || !done.emplace(t).second) return true;
    if (t == var) return false;
    if (auto ex = t->isa<Extract>(); ex && ex->tuple() == var) return ex->index()->isa<Lit>();
    return std::ranges::all_of(t->deps(), [&](const Def* d) { return only_projs(d, var, done); });
}

/// The projection of @p type's binder that names its dead @p i-th component, if anything refers to it.
const Def* binder_proj(const Def* type, size_t n, size_t i) {
    if (auto [_, var] = type->isa_binder<Sigma>(); var) return var->proj(n, i);
    return nullptr;
}

/// That sugar folds the domain into `[<params>, Cn <ret>]`, so a *flat* domain with a trailing Cn stays a `con`.
bool has_fun_shape(const Lam* lam) {
    auto pi = lam->type();
    if (Lam::isa_returning(lam) && num_binders(pi->dom()) == 2)
        if (auto ret = pi->dom(2, 1)) return Pi::isa_basicblock(ret) != nullptr; // a frozen World yields no projection
    return false;
}

} // namespace

bool is_anx_nom(const Def* def) { return def->isa<Nom>() && def->sym().view().contains('.'); }

Def* isa_decl(const Def* def) {
    if (auto mut = def->isa_mut()) {
        if (mut->isa<Hole>()) return nullptr; // a Hole stands for what it was set to, or for `?`
        if (mut->is_external() || mut->isa<Lam>() || (mut->sym() && mut->sym() != '_')) return mut;
    }
    return nullptr;
}

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

nat_t num_binders(const Def* type) {
    if (auto seq = type->isa<Seq>(); seq && seq->shape().is_fused()) return 1;
    return type->num_tprojs();
}

LamKind lam_kind(const Lam* lam, bool fun) {
    return fun ? LamKind::Fun : Lam::isa_cn(lam) ? LamKind::Con : LamKind::Lam;
}

std::string_view decl_kw(LamKind kind) {
    switch (kind) {
        case LamKind::Lam: return "lam";
        case LamKind::Con: return "con";
        case LamKind::Fun: return "fun";
    }
    std::unreachable();
}

/*
 * Namer
 */

void Namer::plain(std::span<const std::string> reserved) {
    plain_ = true;
    taken_.insert(reserved.begin(), reserved.end());
}

std::string Namer::annex(Sym annex) const {
    auto sym = annex.view(); // a short Sym is stored inline, so this view must not outlive @p annex
    if (!self_.empty() && sym.starts_with(self_)) {
        sym.remove_prefix(self_.size());
        if (!mod_.empty() && sym.starts_with(mod_)) sym.remove_prefix(mod_.size());
    }
    return std::string(sym);
}

std::string Namer::fresh(const std::string& base) {
    auto sep  = base == "_" ? "" : "_";
    auto& i   = counters_[base];
    auto name = std::string();
    do
        name = std::format("{}{}{}", base, sep, ++i);
    while (taken_.contains(name));
    taken_.emplace(name);
    return name;
}

std::string Namer::pick(const Def* def) {
    auto sym  = def->sym();
    auto base = sym && sym != '_' && ast::Lexer::is_id(sym.view()) ? sym.str() : "_"s;
    // A numbered variant of an identifier is an identifier and never a keyword.
    auto name = base == "_" || def->world().driver().keys().find(sym) || taken_.contains(base) ? fresh(base) : base;
    taken_.emplace(name);
    return bind(def, name);
}

void Namer::unname(const Var* var) {
    std::erase_if(names_, [var](const auto& entry) {
        const Def* root = entry.first;
        while (auto ex = root->isa<Extract>())
            root = ex->tuple();
        return root == var;
    });
}

std::string Namer::name(const Def* def) {
    if (auto ex = def->isa<Extract>())
        if (rets_.contains(ex->tuple()) && Lit::isa(ex->index()) == 1) return "return"s;
    if (plain_) return pick(def);
    if (auto sym = def->sym(); sym && sym != '_' && PlainNames::claim(def->world().driver(), sym, def->gid()))
        return sym.str();
    return def->unique_name();
}

/*
 * Names & queries
 */

std::string Printer::name(const Def* def) {
    if (auto n = namer_.lookup(def)) return *n;
    if (auto alias = fe::lookup(aliases_, def)) return name(alias);
    if (auto proj = alias_proj(def)) return namer_.bind(def, name(proj));
    return namer_.name(def);
}

std::string Printer::id(const Def* def) {
    if (def->is_external() || (!def->is_set() && def->isa<Lam>())) return def->sym().str();
    return name(def);
}

const std::pair<Sym, Args>* Printer::isa_inst(const Def* def) const {
    return def->isa<Nom>() ? fe::lookup(plan_.insts, def) : nullptr;
}

bool Printer::has_decl(const Def* def) const {
    return isa_decl(def) || (def->isa<Nom>() && !is_anx_nom(def) && !isa_inst(def));
}

bool Printer::is_full(const Def* def) const {
    // Only a `nom` declaration spells out a Nom; World::dump declares the annex ones.
    if (def->isa<Nom>()) return is_anx_nom(def) || isa_inst(def);
    if (plan_.inlined.contains(def)) return true;
    if (in_header_ && plan_.header.contains(def)) return true;
    if (auto mut = def->isa_mut()) return !isa_decl(mut);

    if (def->is_closed()) return true;

    if (auto app = def->isa<App>()) {
        if (app->type()->isa<Pi>()) return true; // a curried App prints Full
        if (app->type()->isa<Type>()) return true;
        if (app->callee()->isa<Axm>()) return app->callee_type()->num_doms() <= 1;
        return false;
    }

    return true;
}

bool Printer::isa_fun(const Lam* lam) const {
    auto var = lam->has_var();
    return has_fun_shape(lam) && !(var && plan_.whole_vars.contains(var));
}

/*
 * Patterns
 */

/// @p def's @p i-th of @p n projections, if it exists and the dump reaches it.
const Def* Printer::live_proj(const Def* def, nat_t n, nat_t i) const {
    auto proj = def->proj(n, i);
    return proj && plan_.live && !plan_.live->contains(proj) ? nullptr : proj;
}

/// Did a pattern keep @p def whole?
bool Printer::is_opaque(const Def* def) const {
    auto split = fe::lookup(split_, def);
    return split && !*split;
}

/// The projection of the aliased Var that @p def - a projection of a Pi's Var - stands for; see Printer::aliases_.
const Def* Printer::alias_proj(const Def* def) const {
    if (auto ex = def->isa<Extract>())
        if (auto var = fe::lookup(aliases_, ex->tuple()); var && !is_opaque(var))
            if (auto l = Lit::isa(ex->index())) return live_proj(var, ex->tuple()->num_tprojs(), *l);
    return nullptr;
}

/// Whether @p ex prints as a bare name rather than `tuple#index`.
bool Printer::by_name(const Extract* ex) const {
    if (!ex->index()->isa<Lit>()) return false;
    if (expr_) return ex->tuple()->isa<Var>();
    if (bound_.contains(ex)) return true;
    // A Var's component prints by name - unless the pattern that would have bound that name kept the Var whole.
    if (is_opaque(ex->tuple())) return false;
    // An aliased Var's component the Lam's Var does not hand out is spelled via the Lam's Var instead.
    if (aliases_.contains(ex->tuple()) && !alias_proj(ex)) return false;
    return ex->tuple()->isa<Var>() || split_.contains(ex->tuple());
}

/// May @p def of @p type be taken apart into @p n components?
/// A component the frozen World does not hand out has no name, so it has to fall back to @p type - and that
/// spells a *dependent* component with the domain's binders instead of this pattern's. That is where
/// destructuring stops: @p def stays opaque and its components print as `def#i` rather than by a dangling name.
bool Printer::destructible(const Def* def, const Def* type, size_t n) const {
    if (!def || n <= 1) return false;
    auto var  = type->isa_mut<Sigma>() ? type->has_var() : nullptr;
    auto done = DefSet();

    for (size_t i = 0; i != n; ++i) {
        if (live_proj(def, n, i)) continue; // it exists and hence brings its own name and type
        auto t = type->proj(n, i);
        if (!t || (var && !only_projs(t, var, done))) return false;
    }
    return true;
}

bool Printer::split(const Def* def, const Def* type, size_t n) {
    if (expr_ || !def) return destructible(def, type, n);
    if (auto res = fe::lookup(split_, def)) return *res;
    auto res = destructible(def, type, n);
    // Def::num_tprojs keeps a Seq above Flags::scalarize_threshold whole, so its components have no name either.
    if (res || n > 1 || def->num_projs() > 1) split_.emplace(def, res);
    return res;
}

/// Taken apart, a dependent Sigma no longer binds its Var itself; see Printer::aliases_.
void Printer::alias_sigma(const Def* type, const Def* def) {
    if (auto [_, var] = type->isa_binder<Sigma>(); !expr_ && var) aliases_.emplace(var, def);
}

/// `x: T` - or `_: T` without a @p def to name.
void Printer::whole(std::ostream& os, const Def* def, const Def* type) {
    if (def) return std::print(os, "{}: {}", name(def), op(type));
    std::print(os, "_: {}", op(type));
}

/// The first @p limit of @p def%'s @p n components as sub-patterns; @p def may be `nullptr`.
void Printer::ptrns(std::ostream& os, const Def* def, const Def* type, size_t n, size_t limit, bool brckt) {
    for (auto sep = ""; auto i : std::views::iota(size_t(0), limit)) {
        auto proj = def ? live_proj(def, n, i) : nullptr;
        os << sep;
        // A projection's own type is the one where the binder has already been substituted for this Var.
        ptrn(os, proj, proj ? proj->type() : type->proj(n, i), brckt, binder_proj(type, n, i));
        sep = ", ";
    }
}

/// @p brckt selects the `[]` sub-patterns of a type over the `()` sub-patterns of a Lam's parameter list.
/// @p binder names a dead component that a dependent Sigma refers to; see binder_proj.
void Printer::ptrn(std::ostream& os, const Def* def, const Def* type, bool brckt, const Def* binder) {
    if (!def) {
        if (!binder || expr_) return whole(os, nullptr, type);
        bound_.emplace(binder);
        return whole(os, binder, type);
    }

    auto n = def->num_tprojs();
    // Unless some component exists, nothing refers to one and the whole reads back the same.
    auto used = std::ranges::any_of(std::views::iota(size_t(0), n), [&](size_t i) { return live_proj(def, n, i); });
    if (!used || !split(def, type, n)) return whole(os, def, type);
    alias_sigma(type, def);
    // The alias keeps a name for the entity as a whole - which the body may well refer to.
    bndr(os, brckt ? Delim::type() : Delim::ptrn(), def, type, n, n, true, true);
}

void Printer::bndr(std::ostream& os,
                   Delim delim,
                   const Def* def,
                   const Def* type,
                   size_t num,
                   size_t limit,
                   bool parts,
                   bool alias) {
    if (limit == 0 && !delim.brckt) return (void)(os << delim.l << delim.r);

    os << delim.l;
    if (parts)
        ptrns(os, def, type, num, limit, delim.brckt);
    else
        whole(os, def, type);
    os << delim.r;
    if (parts && alias && def) std::print(os, " as {}", name(def));
}

/// A Lam's parameter list, plus its filter where that is not the default for its position.
/// @p last marks a curried chain's final Lam: only that one hides a `fun`'s `ret` and defaults its filter to `ff`.
void Printer::lam_ptrn(std::ostream& os, Lam* lam, LamKind kind, bool last, bool alias) {
    auto num   = num_binders(lam->type()->dom());
    auto limit = kind == LamKind::Fun && last ? num - 1 : num;
    auto var   = lam->has_var();
    // A parameter list only comes apart if every parameter is there to be named - unless a `fun` hides its `ret`.
    auto parts = limit != num || (limit != 0 && split(var, lam->dom(), num));
    if (!expr_ && var) {
        if (auto pi_var = lam->type()->has_var()) aliases_.emplace(pi_var, var);
        // Kept whole, the domain prints as a Sigma that binds its Var itself.
        if (parts) alias_sigma(lam->dom(), var);
    }
    bndr(os, Delim::ptrn(lam->type()->is_implicit()), var, lam->type()->dom(), num, limit, parts, alias);
    auto& w = lam->world();
    if (auto dflt = last && kind != LamKind::Lam ? w.lit_ff() : w.lit_tt(); lam->filter() != dflt)
        std::print(os, "@({})", op(lam->filter()));
}

/// A Lam's result type; a `con` has none and a `fun` ascribes the domain of its `ret` continuation.
void Printer::lam_codom(std::ostream& os, Lam* lam, LamKind kind) {
    if (kind == LamKind::Fun)
        std::print(os, ": {}", op(lam->ret_dom()));
    else if (kind == LamKind::Lam)
        std::print(os, ": {}", op(lam->type()->codom()));
}

void Printer::rule_ptrn(std::ostream& os, Rule* rule) {
    auto dom = rule->dom();
    auto num = num_binders(dom);
    auto var = rule->has_var();
    bndr(os, Delim::ptrn(), var, dom, num, num, num != 0 && split(var, dom, num), false);
}

void Printer::lam_header(std::ostream& os, fe::View<Lam*> chain, LamKind kind) {
    auto _    = fe::Restore(in_header_, true);
    auto last = chain.back();
    for (auto* lam : chain) {
        os << ' ';
        lam_ptrn(os, lam, kind, lam == last, kind != LamKind::Fun || lam != last);
    }
    lam_codom(os, last, kind);
}

void Printer::ctors(std::ostream& os, const Variant* variant) {
    if (variant->num_ops() == 0) return (void)(os << '|');
    for (auto sep = ""; auto i : std::views::iota(size_t(0), variant->num_ops())) {
        std::print(os, "{}| _{}", sep, i);
        if (has_payload(variant, i)) std::print(os, ": {}", op(variant->op(i)));
        sep = " ";
    }
}

/*
 * Expressions
 */

Prec Printer::prec(const Def* def) const {
    if (auto ex = def->isa<Extract>()) return by_name(ex) ? Prec::Lit : Prec::Extract;
    if (def->isa<Insert>()) return Prec::Ins;
    if (def->isa<Join>()) return Prec::Union;
    if (auto inj = def->isa<Inj>()) {
        if (auto variant = inj->type()->isa<Variant>())
            return has_payload(variant, inj->index()) ? Prec::App : Prec::Extract;
        return Prec::Inj;
    }
    if (def->isa<Name>()) return Prec::Inj;
    if (def->isa<Struc>()) return Prec::Prefix;
    if (def->isa<Variant>()) return Prec::Lit; // always parenthesized
    if (isa_inst(def) || def->isa<Reform>()) return Prec::App;
    // `Cn e` parses its domain at Prec::Bot, so it swallows whatever follows and only ever fits a closed context.
    if (auto pi = def->isa<Pi>()) return Pi::isa_cn(pi) ? Prec::Bot : Prec::Arrow;
    if (def->isa<Lam>() || def->isa<Match>())
        return Prec::Bot; // a λ- or match-expression swallows what follows it, too
    if (auto app = def->isa<App>()) return isa_idx_name(app) ? Prec::Lit : Prec::App;
    return Prec::Lit;
}

bool Printer::needs_parens(Op d) const {
    if (!is_full(*d)) return false;

    auto child_prec = prec(*d);
    if (child_prec < d.prec()) return true;
    if (child_prec > d.prec()) return false;

    switch (prec_assoc(d.prec())) {
        case Assoc::R: return d.is_left();
        case Assoc::L: return !d.is_left();
        case Assoc::N: return false;
    }
    std::unreachable();
}

std::ostream& operator<<(std::ostream& os, Op op) {
    op.printer_.print(os, op);
    return os;
}

void Printer::print(std::ostream& os, Op d) {
    if (*d == nullptr) return (void)(os << "<nullptr>");
    if (d.full() || is_full(*d)) return print_full(os, d);
    os << id(*d);
}

/// A Seq's shape the way the surface syntax spells it: one axis per dimension, `2, 3` for a fused `(2, 3)`.
/// Falls back to the shape as a whole - also a legal spelling - if an axis does not exist as a Def.
void Printer::shape(std::ostream& os, const Seq* seq) {
    auto s = seq->shape();
    if (auto r = s.rank(); r && *r > 1)
        if (auto axes = s->projs(*r); !std::ranges::contains(axes, nullptr)) return (void)(os << ops(axes));
    os << op(*s);
}

/// As above but each axis binds its index - `i: 2, j: 3`; a fused Seq binds one Var per axis.
void Printer::shape(std::ostream& os, const Seq* seq, const Def* var) {
    if (!var) return shape(os, seq);
    auto s       = seq->shape();
    auto r       = s.rank();
    auto extents = DefVec();
    if (r && *r > 1)
        for (auto i : std::views::iota(nat_t(0), *r))
            extents.emplace_back(s[i]);
    if (extents.empty() || std::ranges::contains(extents, nullptr)) return std::print(os, "{}: {}", op(var), op(*s));

    for (auto sep = ""; auto i : std::views::iota(nat_t(0), *r)) {
        os << sep;
        // A projection the body never mentions does not exist as a Def and hence has no name of its own.
        if (auto axis = live_proj(var, *r, i))
            os << op(axis);
        else
            os << '_';
        std::print(os, ": {}", op(extents[i]));
        sep = ", ";
    }
}

void Printer::print_inst(std::ostream& os, Op d, const std::pair<Sym, Args>& inst) {
    os << namer_.annex(inst.first);
    for (const auto& [arg, implicit] : inst.second) {
        os << (implicit ? " @ " : " ");
        if (arg.size() == 1)
            os << d.r(arg.front(), Prec::App);
        else
            std::print(os, "({})", ops(arg));
    }
}

void Printer::print_lit(std::ostream& os, Op d, const Lit* lit) {
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
            if (*s == 2) return std::print(os, "{}", lit->get<bool>() ? "tt" : "ff");
            if (auto suffix = idx_name(*s)) return std::print(os, "{}{}", lit->get(), *suffix);
            std::print(os, "{}", lit->get());
            auto digits = std::to_string(*s);
            if (d->world().flags().ascii) {
                os << '_' << digits;
            } else {
                // Raw UTF-8 units of a subscript - bytes to stream, not numbers to format.
                for (auto digit : digits)
                    os << "\xE2\x82" << char(0x80 + digit - '0');
            }
            return;
        }
    }
    std::print(os, "{}:{}", lit->get(), d.r(lit->type(), Prec::Lit));
}

void Printer::print_insert(std::ostream& os, Op d, const Insert* ins) {
    auto tup = d.l(ins->tuple(), Prec::Extract);
    // `←` updates the whole `#`-path, so an Extract target needs parens to re-parse.
    if (auto ex = ins->tuple()->isa<Extract>(); ex && !by_name(ex))
        std::print(os, "({})", tup);
    else
        std::print(os, "{}", tup);
    std::print(os, "#{} ← {}", d.r(ins->index(), Prec::Extract), d.r(ins->value(), Prec::Ins));
}

void Printer::print_pi(std::ostream& os, Op d, const Pi* pi) {
    auto arw = d->world().flags().ascii ? "->" : "→";
    if (auto var = pi->has_var(); var && pi->codom()->has_free_var(var)) {
        auto n = var->num_tprojs();
        bndr(os, Delim::type(pi->is_implicit()), var, pi->dom(), n, n, split(var, pi->dom(), n), true);
        return std::print(os, " {} {}", arw, d.r(pi->codom(), Prec::Arrow));
    }
    if (Pi::isa_cn(pi)) return std::print(os, "Cn {}", d.op(pi->dom()));
    if (pi->is_implicit())
        return std::print(os, "{{_: {}}} {} {}", d.op(pi->dom()), arw, d.r(pi->codom(), Prec::Arrow));
    std::print(os, "{} {} {}", d.l(pi->dom(), Prec::Arrow), arw, d.r(pi->codom(), Prec::Arrow));
}

/// A Lam that has no declaration of its own is a λ-expression.
void Printer::print_lam(std::ostream& os, Lam* lam) {
    auto kind = lam_kind(lam, isa_fun(lam));
    std::print(os, "{}", kind == LamKind::Fun ? "fn " : kind == LamKind::Con ? "cn " : "λ ");
    lam_ptrn(os, lam, kind, true, true);
    lam_codom(os, lam, kind);
    // Like any tail, the body is spelled out - there is no block here that could `let`-bind it.
    std::print(os, " = {}", tail(lam->body()));
}

void Printer::print_app(std::ostream& os, Op d, const App* app) {
    if (auto name = isa_idx_name(app)) return std::print(os, "{}", *name);

    if (Pi::isa_implicit(app->callee()->unfold_type())) {
        // An argument that is still a Hole is what the parser inserts anyway - and `?` has no spelling.
        if (app->arg()->has_hole()) return std::print(os, "{}", d.op(app->callee(), d.prec(), d.is_left()));
        // Spelling out an implicit argument needs `@`; juxtaposition would insert a Hole in front of it.
        return std::print(os, "{} @ {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
    }
    std::print(os, "{} {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
}

void Printer::print_sigma(std::ostream& os, Op d, const Sigma* sigma) {
    auto var = sigma->has_var();
    os << '[';
    for (size_t i = 0; auto op : sigma->ops()) {
        if (i != 0) os << ", ";
        if (auto v = var ? var->proj(sigma->num_ops(), i) : nullptr)
            std::print(os, "{}: {}", d.op(v), d.op(op));
        else
            os << d.op(op);
        ++i;
    }
    os << ']';
}

void Printer::print_inj(std::ostream& os, Op d, const Inj* inj) {
    if (auto variant = inj->type()->isa<Variant>()) {
        std::print(os, "{}#{}", d.l(variant, Prec::Extract), inj->index());
        if (has_payload(variant, inj->index())) std::print(os, " {}", d.r(inj->value(), Prec::App));
        return;
    }
    std::print(os, "{} inj {}", d.l(inj->value(), Prec::Inj), d.r(inj->type(), Prec::Inj));
}

void Printer::print_match(std::ostream& os, Op d, const Match* match) {
    std::print(os, "match {} with", d.op(match->scrutinee()));
    auto variant = match->scrutinee()->type()->isa<Variant>();
    ++tab_;
    for (size_t i = 0; auto arm : match->arms()) {
        auto dom = arm->type()->as<Pi>()->dom();
        std::print(os, "\n{}| ", tab_);
        // A variant's arms are its constructors, in order; a constructor without payload binds nothing.
        if (variant) std::print(os, "_{}", i++);
        auto binds = !variant || dom != d->world().sigma();
        if (binds && variant) os << ' ';
        if (plan_.arms.contains(arm)) {
            auto lam = arm->as_mut<Lam>();
            if (auto var = lam->has_var(); var && !spelled_.emplace(lam).second) namer_.unname(var);
            if (binds) ptrn(os, lam->has_var(), dom);
            // Another `match` would swallow the arms that follow.
            std::print(os, " => {}", d.op(lam->body(), Prec::Where));
        } else {
            auto x = binds ? namer_.fresh("_") : "()"s;
            if (binds) std::print(os, "{}: {}", x, d.op(dom));
            std::print(os, " => {} {}", d.l(arm, Prec::App), x);
        }
    }
    --tab_;
}

/// Streams @p d with all of its operands; Printer::is_full decides who gets here.
void Printer::print_full(std::ostream& os, Op d) {
    if (auto hole = d->isa_mut<Hole>()) {
        if (hole->is_set()) return std::print(os, "{}", d.op(hole->op()));
        return std::print(os, "?");
    }
    if (auto mut = d->isa_mut(); mut && !mut->is_set()) return std::print(os, "unset");
    if (needs_parens(d)) return std::print(os, "({})", full(*d));

    bool ascii = d->world().flags().ascii;
    auto al    = ascii ? "<<" : "«";
    auto ar    = ascii ? ">>" : "»";
    auto pl    = ascii ? "(<" : "‹";
    auto pr    = ascii ? ">)" : "›";

    if (auto type = d->isa<Type>()) {
        if (auto level = Lit::isa(type->level()); level && !ascii) {
            if (level == 0) return std::print(os, "*");
            if (level == 1) return std::print(os, "□");
        }
        return std::print(os, "Type {}", d.r(type->level(), Prec::App));
    }
    if (auto reform = d->isa<Reform>()) return std::print(os, "Rule {}", d.r(reform->dom(), Prec::App));
    if (d->isa<Level>()) return std::print(os, "Level");
    if (d->isa<Nat>()) return std::print(os, "Nat");
    if (d->isa<Idx>()) return std::print(os, "Idx");
    if (auto ext = d->isa<Ext>()) {
        auto sym = ext->isa<Bot>() ? (ascii ? "bot" : "⊥") : (ascii ? "top" : "⊤");
        return std::print(os, "{}:{}", sym, d.r(ext->type(), Prec::Lit));
    }
    if (d->isa<Axm>() || is_anx_nom(*d)) return std::print(os, "{}", namer_.annex(d->sym()));
    if (auto inst = isa_inst(*d)) return print_inst(os, d, *inst);
    if (auto lit = d->isa<Lit>()) return print_lit(os, d, lit);
    if (auto ex = d->isa<Extract>()) {
        if (by_name(ex)) return std::print(os, "{}", name(ex));
        return std::print(os, "{}#{}", d.l(ex->tuple(), Prec::Extract), d.r(ex->index(), Prec::Extract));
    }
    if (auto ins = d->isa<Insert>()) return print_insert(os, d, ins);
    if (auto var = d->isa<Var>()) return std::print(os, "{}", name(var));
    if (auto pi = d->isa<Pi>()) return print_pi(os, d, pi);
    if (auto lam = d->isa_mut<Lam>()) return print_lam(os, lam);
    if (auto app = d->isa<App>()) return print_app(os, d, app);
    if (auto sigma = d->isa<Sigma>()) return print_sigma(os, d, sigma);
    if (auto tuple = d->isa<Tuple>()) return std::print(os, "({})", ops(tuple->ops()));
    if (auto seq = d->isa<Seq>()) {
        auto arr = seq->isa<Arr>();
        os << (arr ? al : pl);
        shape(os, seq, seq->has_var());
        return std::print(os, "; {}{}", d.op(seq->body()), arr ? ar : pr);
    }
    if (auto proxy = d->isa<Proxy>()) return std::print(os, "(proxy#{} {})", proxy->tag(), ops(proxy->ops()));
    if (auto join = d->isa<Join>()) return std::print(os, "{}", ops(join->ops(), " ∪ ", Prec::Union));
    if (auto variant = d->isa<Variant>()) {
        os << '(';
        ctors(os, variant);
        return (void)(os << ')');
    }
    if (auto inj = d->isa<Inj>()) return print_inj(os, d, inj);
    if (auto name = d->isa<Name>())
        return std::print(os, "{} inj {}", d.l(name->op(), Prec::Inj), d.r(name->nom(), Prec::Inj));
    if (auto struc = d->isa<Struc>()) return std::print(os, "#{}", d.r(struc->op(), Prec::Prefix));
    if (auto single = d->isa<Single>()) return std::print(os, "{}{}{}", al, d.op(single->op()), ar);
    if (auto wrap = d->isa<Wrap>()) return std::print(os, "{}{}{}", pl, d.op(wrap->op()), pr);
    if (auto match = d->isa<Match>(); match && !expr_ && match->num_arms() != 0) return print_match(os, d, match);

    auto tag = d->flags() == 0 ? std::string(d->node_name()) : std::format("{}#{}", d->node_name(), d->flags());
    if (d->ops().empty()) return std::print(os, "({})", tag);
    std::print(os, "({} {})", tag, ops(d->ops(), " "));
}

} // namespace mim::dump
