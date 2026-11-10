#include <fstream>
#include <ostream>
#include <ranges>

#include <fe/assert.h>

#include "mim/driver.h"
#include "mim/nest.h"

#include "mim/ast/lexer.h"
#include "mim/ast/tok.h"

using namespace std::literals;

// A dump prints a Def that appears as an operand in one of two ways:
// * Full: spelled out with all of its operands, e.g. `(1, 2, 3)`; see Printer::is_full.
// * By name (see Printer::id): everything else; a `let` - or a declaration of its own for a decl - binds it.
// A mutable that is no decl is printed like an immutable.

namespace mim {

namespace {

using ast::Assoc;
using ast::Prec;
using ast::prec_assoc;

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

/// An `anx nom` carries its qualified `<file>.<name>` sym, which no identifier can spell; see Annexes::attach.
bool is_anx_nom(const Def* def) { return def->isa<Nom>() && def->sym().view().contains('.'); }

Def* isa_decl(const Def* def) {
    if (auto mut = def->isa_mut()) {
        if (mut->isa<Hole>()) return nullptr; // a Hole stands for what it was set to, or for `?`
        if (mut->is_external() || mut->isa<Lam>() || (mut->sym() && mut->sym() != '_')) return mut;
    }
    return nullptr;
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

/// Def::num_tprojs, except that a *fused* Seq stays one binder: dumping freezes the World and its sub-Seqs
/// need not exist as Def%s.
nat_t num_binders(const Def* type) {
    if (auto seq = type->isa<Seq>(); seq && seq->shape().is_fused()) return 1;
    return type->num_tprojs();
}

/// That sugar folds the domain into `[<params>, Cn <ret>]`, so a *flat* domain with a trailing Cn stays a `con`.
bool has_fun_shape(const Lam* lam) {
    auto pi = lam->type();
    if (Lam::isa_returning(lam) && num_binders(pi->dom()) == 2)
        if (auto ret = pi->dom(2, 1)) return Pi::isa_basicblock(ret) != nullptr; // a frozen World yields no projection
    return false;
}

/// The surface syntax a Lam prints with; a `fun` is sugar for a Lam of the shape `[<params>, Cn <ret>]`.
enum class LamKind { Lam, Con, Fun };

/// The kind of @p lam, where @p fun tells whether it may print as a `fun`.
LamKind lam_kind(const Lam* lam, bool fun) {
    return fun ? LamKind::Fun : Lam::isa_cn(lam) ? LamKind::Con : LamKind::Lam;
}

/// The keyword that declares a Lam of that kind.
std::string_view decl_kw(LamKind kind) {
    switch (kind) {
        case LamKind::Lam: return "lam";
        case LamKind::Con: return "con";
        case LamKind::Fun: return "fun";
    }
    std::unreachable();
}

/// Each curried argument of an application and whether its Pi is implicit.
using Args = fe::Vector<std::pair<DefVec, bool>>;

/// An instance of a generic `anx nom` from elsewhere: its annex's qualified name and arguments.
/// Each curried argument holds either the whole argument or the components a re-read dump tuples up again.
using Insts = DefMap<std::pair<Sym, Args>>;

/// What encloses a binder: `()` for a pattern, `[]` for a type, and `{}` for an implicit one of either.
struct Delim {
    static Delim ptrn(bool implicit = false) { return implicit ? Delim{'{', '}', false} : Delim{'(', ')', false}; }
    static Delim type(bool implicit = false) { return implicit ? Delim{'{', '}', true} : Delim{'[', ']', true}; }

    char l, r;
    bool brckt; ///< Whether the sub-patterns are those of a type.
};

/// What a Dumper decided about the Def%s it emits; a Printer of a bare Def has nothing planned.
struct Plan {
    DefSet whole_vars; ///< Var%s used as a whole; see Printer::isa_fun.
    DefSet arms;       ///< Lam%s a `match` spells out as its arms instead of declaring them.
    DefSet header;     ///< Def%s a Lam's header prints inline, as it precedes the block that `let`s them.
    DefSet inlined;    ///< Def%s inside a binder the dump prints *inline* - they have no block to `let` them.
    /// What the dumped roots reach; `nullptr` dumps whatever exists.
    /// A dead projection left behind by normalization must not bind a name a re-read dump would not bind.
    const DefSet* live = nullptr;
    Insts insts;
};

class Printer;

/// A Def to stream - by default as an operand of the expression at hand: Full if Printer::is_full, else by name.
struct Op {
    enum class As {
        Operand,
        Full,   ///< Spelled out, whatever Printer::is_full says.
        Ctors,  ///< A Variant as the list of its constructors.
        Shape,  ///< A Seq's shape, each axis with the index it binds.
        Params, ///< A Rule's parameter list.
    };

    Op l(const Def* def, Prec prec = Prec::Bot) const { return {printer, def, prec, true}; }
    Op r(const Def* def, Prec prec = Prec::Bot) const { return {printer, def, prec, false}; }
    Op op(const Def* def, Prec prec = Prec::Bot, bool is_left = false) const { return {printer, def, prec, is_left}; }

    const Def* operator->() const { return def; }
    const Def* operator*() const { return def; }

    Printer& printer;
    const Def* def;
    Prec prec    = Prec::Bot;
    bool is_left = false;
    As as        = As::Operand;
};

std::ostream& operator<<(std::ostream&, Op);

} // namespace
} // namespace mim

#ifndef DOXYGEN
template<>
struct std::formatter<mim::Op> : fe::ostream_formatter {};
#endif

namespace mim {
namespace {

/// Streams Def%s in Mim syntax.
class Printer {
public:
    /// @p expr formats a bare Def - Def::Dump::Expr or a diagnostic: no pattern binds a name beyond its own spelling.
    explicit Printer(bool expr = false)
        : expr_(expr) {}

    /// @name Getters
    ///@{
    Plan& plan() { return plan_; }
    const Plan& plan() const { return plan_; }
    fe::Tab& tab() { return tab_; }
    ///@}

    /// @name Operands
    ///@{
    Op op(const Def* def, Prec prec = Prec::Bot) { return {*this, def, prec}; }
    Op full(const Def* def) { return {*this, def, Prec::Bot, false, Op::As::Full}; }
    Op ctors(const Variant* variant) { return {*this, variant, Prec::Bot, false, Op::As::Ctors}; }
    Op shape(const Seq* seq) { return {*this, seq, Prec::Bot, false, Op::As::Shape}; }
    Op params(const Rule* rule) { return {*this, rule, Prec::Bot, false, Op::As::Params}; }
    /// The right-hand side of a declaration; only a bare variant may refer back to the name it declares.
    Op rhs(Op op) { return op->isa<Variant>() ? ctors(op->as<Variant>()) : op; }
    /// A block's tail has no `let`, so it is spelled out unless it has a declaration of its own.
    Op tail(const Def* def) { return has_decl(def) ? op(def) : full(def); }
    auto ops(const auto& range, const char* sep = ", ", Prec prec = Prec::Bot) {
        return fe::Join(range | std::views::transform([this, prec](auto def) { return Op{*this, def, prec}; }), sep);
    }
    ///@}

    /// @name Names
    ///@{
    /// Switches to plain names; @p reserved are names the dump refers to verbatim and hence must not pick.
    void plain(std::span<const std::string> reserved) {
        plain_ = true;
        taken_.insert(reserved.begin(), reserved.end());
    }

    /// `<world name>.`: the dumped file's own annexes are spelled without it.
    void self(std::string self) { self_ = std::move(self); }
    /// `<mod>.` while declaring the Axm%s of that `mod`, which does not see its own name.
    void mod(std::string mod) { mod_ = std::move(mod); }
    /// Names @p def @p name unless it has a name already; yields the name it goes by.
    const std::string& bind(const Def* def, std::string name) {
        return names_.emplace(def, std::move(name)).first->second;
    }
    /// The last component of @p var - a `fun`'s - is the `ret` continuation it calls `return`, whatever its Def::sym.
    /// Keyed by Var, because the projection itself need not exist as a Def while the World is frozen.
    void ret(const Def* var) { rets_.emplace(var); }
    /// Def::unique_name - or a plain name while dumping a World or formatting a diagnostic, where a gid is noise.
    std::string name(const Def* def) {
        if (auto n = fe::lookup(names_, def)) return *n;
        if (auto alias = fe::lookup(aliases_, def)) return name(alias);
        if (auto proj = alias_proj(def)) return bind(def, name(proj));
        if (auto ex = def->isa<Extract>(); ex && rets_.contains(ex->tuple()) && Lit::isa(ex->index()) == 1)
            return "return"s;
        if (plain_) return pick(def);
        if (auto sym = def->sym(); sym && sym != '_' && PlainNames::claim(def->world().driver(), sym, def->gid()))
            return sym.str();
        return def->unique_name();
    }

    std::string id(const Def* def) {
        if (def->is_external() || (!def->is_set() && def->isa<Lam>())) return def->sym().str();
        return name(def);
    }

    ///@}

    /// @name Queries
    ///@{
    const std::pair<Sym, Args>* isa_inst(const Def* def) const {
        return def->isa<Nom>() ? fe::lookup(plan_.insts, def) : nullptr;
    }

    /// Does the Def print by name, bound by a declaration of its own - be it a decl or a `nom`?
    bool has_decl(const Def* def) const {
        return isa_decl(def) || (def->isa<Nom>() && !is_anx_nom(def) && !isa_inst(def));
    }

    /// Does the Def always print Full - instead of by name?
    bool is_full(const Def* def) const {
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

    /// Is the Lam sugar for a `fun`?
    /// A `fun` binds only its parameters, so a Var used as a whole needs the `con` spelling.
    bool isa_fun(const Lam* lam) const {
        auto var = lam->has_var();
        return has_fun_shape(lam) && !(var && plan_.whole_vars.contains(var));
    }

    ///@}

    /// @name Patterns
    ///@{
    /// The parameter lists of a curried @p chain and its result type.
    void lam_header(std::ostream& os, fe::View<Lam*> chain, LamKind kind) {
        auto _    = fe::Restore(in_header_, true);
        auto last = chain.back();
        for (auto* lam : chain) {
            os << ' ';
            lam_ptrn(os, lam, kind, lam == last, kind != LamKind::Fun || lam != last);
        }
        lam_codom(os, last, kind);
    }

    ///@}

private:
    /// @name Names
    ///@{
    std::string pick(const Def* def) {
        auto sym  = def->sym();
        auto base = sym && sym != '_' && ast::Lexer::is_id(sym.view()) ? sym.str() : "_"s;
        // A numbered variant of an identifier is an identifier and never a keyword.
        auto name = base == "_" || def->world().driver().keys().find(sym) || taken_.contains(base) ? fresh(base) : base;
        taken_.emplace(name);
        return bind(def, name);
    }

    /// A numbered variant of @p base that no other name of the dump takes.
    std::string fresh(const std::string& base) {
        auto sep  = base == "_" ? "" : "_";
        auto& i   = counters_[base];
        auto name = std::string();
        do
            name = std::format("{}{}{}", base, sep, ++i);
        while (taken_.contains(name));
        taken_.emplace(name);
        return name;
    }

    /// Forgets the names of @p var and its projections, so that the next spelling picks fresh ones.
    void unname(const Var* var) {
        std::erase_if(names_, [var](const auto& entry) {
            const Def* root = entry.first;
            while (auto ex = root->isa<Extract>())
                root = ex->tuple();
            return root == var;
        });
    }

    /// The name an annex prints by: the dumped file's own annexes drop the `<file>.` - and the `<mod>.` - prefix.
    std::string annex(Sym annex) const {
        auto sym = annex.view(); // a short Sym is stored inline, so this view must not outlive @p annex
        if (!self_.empty() && sym.starts_with(self_)) {
            sym.remove_prefix(self_.size());
            if (!mod_.empty() && sym.starts_with(mod_)) sym.remove_prefix(mod_.size());
        }
        return std::string(sym);
    }

    ///@}

    /// @name Expressions
    ///@{
    Prec prec(const Def* def) const {
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

    bool needs_parens(Op d) const {
        if (!is_full(*d)) return false;

        auto child_prec = prec(*d);
        if (child_prec < d.prec) return true;
        if (child_prec > d.prec) return false;

        switch (prec_assoc(d.prec)) {
            case Assoc::R: return d.is_left;
            case Assoc::L: return !d.is_left;
            case Assoc::N: return false;
        }
        std::unreachable();
    }

    /// Streams @p d with all of its operands; Printer::is_full decides who gets here.
    void print_full(std::ostream& os, Op d) {
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
        if (d->isa<Axm>() || is_anx_nom(*d)) return std::print(os, "{}", annex(d->sym()));
        if (auto inst = isa_inst(*d)) {
            os << annex(inst->first);
            for (const auto& [arg, implicit] : inst->second) {
                os << (implicit ? " @ " : " ");
                if (arg.size() == 1)
                    os << d.r(arg.front(), Prec::App);
                else
                    std::print(os, "({})", ops(arg));
            }
            return;
        }
        if (auto lit = d->isa<Lit>()) return print_lit(os, d, lit);
        if (auto ex = d->isa<Extract>()) {
            if (by_name(ex)) return std::print(os, "{}", name(ex));
            return std::print(os, "{}#{}", d.l(ex->tuple(), Prec::Extract), d.r(ex->index(), Prec::Extract));
        }
        if (auto ins = d->isa<Insert>()) {
            // `←` updates the whole `#`-path, so an Extract target needs parens to re-parse.
            auto ex    = ins->tuple()->isa<Extract>();
            auto paren = ex && !by_name(ex);
            os << (paren ? "(" : "") << d.l(ins->tuple(), Prec::Extract) << (paren ? ")" : "");
            return std::print(os, "#{} ← {}", d.r(ins->index(), Prec::Extract), d.r(ins->value(), Prec::Ins));
        }
        if (auto var = d->isa<Var>()) return std::print(os, "{}", name(var));
        if (auto pi = d->isa<Pi>()) return print_pi(os, d, pi);
        if (auto lam = d->isa_mut<Lam>()) {
            // A Lam that has no declaration of its own is a λ-expression.
            auto kind = lam_kind(lam, isa_fun(lam));
            os << (kind == LamKind::Fun ? "fn " : kind == LamKind::Con ? "cn " : "λ ");
            lam_ptrn(os, lam, kind, true, true);
            lam_codom(os, lam, kind);
            // Like any tail, the body is spelled out - there is no block here that could `let`-bind it.
            return std::print(os, " = {}", tail(lam->body()));
        }
        if (auto app = d->isa<App>()) return print_app(os, d, app);
        if (auto sigma = d->isa<Sigma>()) return print_sigma(os, d, sigma);
        if (auto tuple = d->isa<Tuple>()) return std::print(os, "({})", ops(tuple->ops()));
        if (auto seq = d->isa<Seq>()) {
            auto arr = seq->isa<Arr>();
            return std::print(os, "{}{}; {}{}", arr ? al : pl, shape(seq), d.op(seq->body()), arr ? ar : pr);
        }
        if (auto proxy = d->isa<Proxy>()) return std::print(os, "(proxy#{} {})", proxy->tag(), ops(proxy->ops()));
        if (auto join = d->isa<Join>()) return std::print(os, "{}", ops(join->ops(), " ∪ ", Prec::Union));
        if (auto variant = d->isa<Variant>()) return std::print(os, "({})", ctors(variant));
        if (auto inj = d->isa<Inj>()) {
            auto variant = inj->type()->isa<Variant>();
            if (!variant) return std::print(os, "{} inj {}", d.l(inj->value(), Prec::Inj), d.r(inj->type(), Prec::Inj));
            std::print(os, "{}#{}", d.l(variant, Prec::Extract), inj->index());
            if (has_payload(variant, inj->index())) std::print(os, " {}", d.r(inj->value(), Prec::App));
            return;
        }
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

    void print_lit(std::ostream& os, Op d, const Lit* lit) {
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

    void print_pi(std::ostream& os, Op d, const Pi* pi) {
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

    void print_app(std::ostream& os, Op d, const App* app) {
        if (auto name = isa_idx_name(app)) return std::print(os, "{}", *name);

        if (Pi::isa_implicit(app->callee()->unfold_type())) {
            // An argument that is still a Hole is what the parser inserts anyway - and `?` has no spelling.
            if (app->arg()->has_hole()) return std::print(os, "{}", d.op(app->callee(), d.prec, d.is_left));
            // Spelling out an implicit argument needs `@`; juxtaposition would insert a Hole in front of it.
            return std::print(os, "{} @ {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
        }
        std::print(os, "{} {}", d.l(app->callee(), Prec::App), d.r(app->arg(), Prec::App));
    }

    void print_sigma(std::ostream& os, Op d, const Sigma* sigma) {
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

    void print_match(std::ostream& os, Op d, const Match* match) {
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
                if (auto var = lam->has_var(); var && !spelled_.emplace(lam).second) unname(var);
                if (binds) ptrn(os, lam->has_var(), dom);
                // Another `match` would swallow the arms that follow.
                std::print(os, " => {}", d.op(lam->body(), Prec::Where));
            } else {
                auto x = binds ? fresh("_") : "()"s;
                if (binds) std::print(os, "{}: {}", x, d.op(dom));
                std::print(os, " => {} {}", d.l(arm, Prec::App), x);
            }
        }
        --tab_;
    }

    /// The names of the constructors live in the frontend, so each case prints under its index.
    void ctors(std::ostream& os, const Variant* variant) {
        if (variant->num_ops() == 0) return (void)(os << '|');
        for (auto sep = ""; auto i : std::views::iota(size_t(0), variant->num_ops())) {
            std::print(os, "{}| _{}", sep, i);
            if (has_payload(variant, i)) std::print(os, ": {}", op(variant->op(i)));
            sep = " ";
        }
    }

    /// A Seq's shape the way the surface syntax spells it: one axis per dimension, `2, 3` for a fused `(2, 3)`.
    /// Each axis binds its index - `i: 2, j: 3`; a fused Seq binds one Var per axis.
    /// Falls back to the shape as a whole - also a legal spelling - if an axis does not exist as a Def.
    void shape(std::ostream& os, const Seq* seq) {
        auto s   = seq->shape();
        auto r   = s.rank();
        auto var = seq->has_var();
        if (!var) {
            if (r && *r > 1)
                if (auto axes = s->projs(*r); !std::ranges::contains(axes, nullptr)) return (void)(os << ops(axes));
            return (void)(os << op(*s));
        }

        auto extents = DefVec();
        if (r && *r > 1)
            for (auto i : std::views::iota(nat_t(0), *r))
                extents.emplace_back(s[i]);
        if (extents.empty() || std::ranges::contains(extents, nullptr))
            return std::print(os, "{}: {}", op(var), op(*s));

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

    void params(std::ostream& os, const Rule* rule) {
        auto dom = rule->dom();
        auto num = num_binders(dom);
        auto var = rule->has_var();
        bndr(os, Delim::ptrn(), var, dom, num, num, num != 0 && split(var, dom, num), false);
    }

    ///@}

    /// @name Patterns
    ///@{
    /// @p def's @p i-th of @p n projections, if it exists and the dump reaches it.
    const Def* live_proj(const Def* def, nat_t n, nat_t i) const {
        auto proj = def->proj(n, i);
        return proj && plan_.live && !plan_.live->contains(proj) ? nullptr : proj;
    }

    /// Did a pattern keep @p def whole?
    bool is_opaque(const Def* def) const {
        auto split = fe::lookup(split_, def);
        return split && !*split;
    }

    /// The projection of the aliased Var that @p def - a projection of a Pi's Var - stands for; see Printer::aliases_.
    const Def* alias_proj(const Def* def) const {
        if (auto ex = def->isa<Extract>())
            if (auto var = fe::lookup(aliases_, ex->tuple()); var && !is_opaque(var))
                if (auto l = Lit::isa(ex->index())) return live_proj(var, ex->tuple()->num_tprojs(), *l);
        return nullptr;
    }

    /// Whether @p ex prints as a bare name rather than `tuple#index`.
    bool by_name(const Extract* ex) const {
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
    bool destructible(const Def* def, const Def* type, size_t n) const {
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

    /// Taken apart, a dependent Sigma no longer binds its Var itself; see Printer::aliases_.
    void alias_sigma(const Def* type, const Def* def) {
        if (auto [_, var] = type->isa_binder<Sigma>(); !expr_ && var) aliases_.emplace(var, def);
    }

    /// Whether @p def of @p type comes apart into @p n components; by_name agrees with that from then on.
    bool split(const Def* def, const Def* type, size_t n) {
        if (expr_ || !def) return destructible(def, type, n);
        if (auto res = fe::lookup(split_, def)) return *res;
        auto res = destructible(def, type, n);
        // Def::num_tprojs keeps a Seq above Flags::scalarize_threshold whole, so its components have no name either.
        if (res || n > 1 || def->num_projs() > 1) split_.emplace(def, res);
        return res;
    }

    /// `x: T` - or `_: T` without a @p def to name.
    void whole(std::ostream& os, const Def* def, const Def* type) {
        if (def) return std::print(os, "{}: {}", name(def), op(type));
        std::print(os, "_: {}", op(type));
    }

    /// A binder within @p delim: `(a: A, b: B) as x` if it comes apart into @p parts, `(x: T)` if not.
    /// @p num is how many components it has; @p limit how many of them to print - a returning Lam hides its last.
    void bndr(std::ostream& os,
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

    /// @p brckt selects the `[]` sub-patterns of a type over the `()` sub-patterns of a Lam's parameter list.
    /// @p binder names a dead component that a dependent Sigma refers to; see binder_proj.
    void ptrn(std::ostream& os, const Def* def, const Def* type, bool brckt = false, const Def* binder = nullptr) {
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

    /// The first @p limit of @p def%'s @p n components as sub-patterns; @p def may be `nullptr`.
    void ptrns(std::ostream& os, const Def* def, const Def* type, size_t n, size_t limit, bool brckt) {
        for (auto sep = ""; auto i : std::views::iota(size_t(0), limit)) {
            auto proj = def ? live_proj(def, n, i) : nullptr;
            os << sep;
            // A projection's own type is the one where the binder has already been substituted for this Var.
            ptrn(os, proj, proj ? proj->type() : type->proj(n, i), brckt, binder_proj(type, n, i));
            sep = ", ";
        }
    }

    /// A Lam's parameter list, plus its filter where that is not the default for its position.
    /// @p last marks a curried chain's final Lam: only that one hides a `fun`'s `ret` and defaults its filter to `ff`.
    void lam_ptrn(std::ostream& os, Lam* lam, LamKind kind, bool last, bool alias) {
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
    void lam_codom(std::ostream& os, Lam* lam, LamKind kind) {
        if (kind == LamKind::Fun)
            std::print(os, ": {}", op(lam->ret_dom()));
        else if (kind == LamKind::Lam)
            std::print(os, ": {}", op(lam->type()->codom()));
    }

    ///@}

    bool expr_;
    bool in_header_ = false;
    fe::Tab tab_    = fe::Tab::spaces(); ///< Indentation of the line being printed.
    Plan plan_;
    DefMap<std::string> names_;
    DefSet rets_;
    /// Distinct identifiers picked instead of Def::unique_name, so a re-read dump keeps them as its Def::sym%s.
    ankerl::unordered_dense::set<std::string> taken_;
    ankerl::unordered_dense::map<std::string, size_t> counters_; ///< Next numbered variant to try per base name.
    bool plain_ = false;
    std::string self_;
    std::string mod_;
    DefMap<bool> split_; ///< Whether a pattern took a Def apart, so that its components print by name.
    DefSet bound_;       ///< Projections of a dependent Sigma's Var a pattern binds in place of a dead component.
    DefSet spelled_;     ///< Arms already spelled out; a re-read dump binds fresh names for each further spelling.
    /// A Lam's codomain refers to its Pi's Var - or its dependent domain's - but only the Lam's Var is bound by name.
    DefMap<const Def*> aliases_;

    friend std::ostream& operator<<(std::ostream&, Op);
};

std::ostream& operator<<(std::ostream& os, Op op) {
    auto& p = op.printer;
    if (!op.def) return os << "<nullptr>";
    // clang-format off
    switch (op.as) {
        case Op::As::Ctors:  p.ctors (os, op->as<Variant>()); break;
        case Op::As::Shape:  p.shape (os, op->as<Seq>());     break;
        case Op::As::Params: p.params(os, op->as<Rule>());    break;
        case Op::As::Full:   p.print_full(os, op);            break;
        case Op::As::Operand:
            if (p.is_full(op.def)) p.print_full(os, op);
            else os << p.id(op.def);
    }
    // clang-format on
    return os;
}

/// Recovers the arguments of an instance of a generic Nom by matching it against the generic one.
/// The generic Nom may live in Driver::library, so even closed parts are compared structurally.
class Instantiation {
public:
    /// @p chain is the curry_chain of the annex Lam whose body is the generic Nom.
    explicit Instantiation(const fe::Vector<Lam*>& chain)
        : chain_(chain) {
        for (auto lam : chain)
            if (auto var = lam->has_var()) vars_.emplace(var);
    }

    /// `std::nullopt` if a parameter is not recoverable.
    std::optional<Args> args(const Nom* inst) {
        auto generic = chain_.back()->body()->as<Nom>();
        if (inst == generic || !match(generic->op(), inst->op())) return {};

        auto res = Args();
        for (auto lam : chain_) {
            auto var      = lam->has_var();
            auto implicit = lam->type()->is_implicit();
            if (!var) return {};
            if (auto whole = fe::lookup(subst_, var)) {
                res.emplace_back(DefVec{whole}, implicit);
                continue;
            }
            auto n = Lit::isa(var->arity());
            if (!n || *n < 2) return {};
            auto comps = DefVec(*n, nullptr);
            for (auto [g, i] : subst_)
                if (auto ex = g->isa<Extract>(); ex && ex->tuple() == var) comps[*Lit::isa(ex->index())] = i;
            if (std::ranges::contains(comps, nullptr)) return {};
            res.emplace_back(std::move(comps), implicit);
        }
        return res;
    }

private:
    bool is_param(const Def* def) const {
        if (auto ex = def->isa<Extract>(); ex && ex->index()->isa<Lit>()) def = ex->tuple();
        return vars_.contains(def);
    }

    /// Appends the axes of @p shape that World::seq keeps; `false` for a dynamic rank.
    static bool axes(Shape shape, DefVec& res) {
        if (shape.is_dim()) {
            if (Shape::extent(*shape) != 1) res.emplace_back(*shape);
            return true;
        }
        auto r = shape.rank();
        if (!r) return false;
        for (nat_t k = 0; k != *r; ++k)
            if (auto a = shape[k]; !a)
                return false;
            else if (Shape::extent(a) != 1)
                res.emplace_back(a);
        return true;
    }

    /// Is @p i what World::arr normalizes `«shape; g»` to, with @p shape already in the World of @p i?
    /// Building that Arr would mix in the generic @p g, which may live in Driver::library.
    bool is_arr(Shape shape, const Def* g, const Def* i) {
        auto dims = DefVec();
        if (!axes(shape, dims)) return false;
        auto inst = fe::lookup(subst_, g);
        auto gen  = DefVec();
        if (auto zero = std::ranges::find(dims, 0, [](const Def* a) { return Shape::extent(a); }); zero != dims.end()) {
            dims.erase(zero, dims.end());
            inst = i->world().sigma();
        } else if (inst) {
            for (auto arr = inst->isa_imm<Arr>(); arr; arr = inst->isa_imm<Arr>()) {
                if (!axes(arr->shape(), dims)) return false;
                inst = arr->body();
            }
        } else {
            for (auto arr = g->isa_imm<Arr>(); arr; arr = g->isa_imm<Arr>()) {
                if (!axes(arr->shape(), gen)) return false;
                g = arr->body();
            }
        }

        auto body = [&](const Def* ib) { return inst ? inst == ib : match(g, ib); };
        if (dims.empty() && gen.empty()) return body(i);
        auto arr  = i->isa_imm<Arr>();
        auto idim = DefVec();
        if (!arr || !axes(arr->shape(), idim) || idim.size() != dims.size() + gen.size()) return false;
        for (size_t k = 0, n = dims.size(); k != n; ++k)
            if (dims[k] != idim[k]) return false;
        for (size_t k = 0, n = gen.size(); k != n; ++k)
            if (!match(gen[k], idim[dims.size() + k])) return false;
        return body(arr->body());
    }

    bool match(const Def* g, const Def* i) {
        if (g == i) return true;
        // Only a Level has no type.
        if (!g || !i) return false;
        // A substitution maps each generic subterm to exactly one instance subterm.
        if (auto [j, ins] = subst_.emplace(g, i); !ins) return j->second == i;
        if (is_param(g)) return true;
        if (auto arr = g->isa_imm<Arr>()) {
            auto shape      = fe::lookup(subst_, *arr->shape());
            auto body       = arr->body();
            auto same_shape = shape && i->isa_imm<Arr>() && *i->as<Arr>()->shape() == shape;
            if (shape && (subst_.contains(body) || body->is_closed()) && !same_shape) return is_arr(shape, body, i);
        }
        if (auto sig = g->isa_imm<Sigma>())
            if (auto arr = i->isa_imm<Arr>();
                arr && arr->shape().is_dim() && Lit::isa(*arr->shape()) == sig->num_ops()) {
                for (auto op : sig->ops())
                    if (!match(op, arr->body())) return false;
                return match(g->type(), i->type());
            }
        if (g->node() != i->node() || g->flags() != i->flags() || g->num_ops() != i->num_ops()) return false;
        for (size_t k = 0, n = g->num_ops(); k != n; ++k)
            if (!match(g->op(k), i->op(k))) return false;
        return match(g->type(), i->type());
    }

    const fe::Vector<Lam*>& chain_;
    DefSet vars_;
    DefMap<const Def*> subst_;
};

using Srcs = ankerl::unordered_dense::set<const fe::Src*>;

/// What a block declares at one position: a `let`, a declaration, or - if @p group is set - a `mutual` group.
struct Item {
    const Def* def;
    DefVec group = {};
};

using Block = fe::Vector<Item>;

/// An annex of the dumped file itself; its name is split into the `mod` it nests in and its own name.
struct DumpAnnex {
    std::string mod, name;
    const Def* def;
};

/// Emits Def%s as Mim declarations.
///
/// Dump::Scope and Dump::All place everything into the block of the innermost binder its free Var%s refer to.
/// Dump::Expr asks no analysis at all, and neither do the tooltips of World::dot.
class Dumper {
public:
    /// @p srcs are the files an `import` pulls in: they declare their own content, so a dump must not repeat it.
    Dumper(std::ostream& os, Def::Dump mode, bool typed_let = false, Srcs srcs = {})
        : os_(os)
        , mode_(mode)
        , typed_let_(typed_let)
        , srcs_(std::move(srcs)) {}

    /// Only @p live Def%s - what the dumped roots reach - count as existing.
    void live(const DefSet& live) { plan().live = &live; }
    void insts(Insts insts) { plan().insts = std::move(insts); }
    /// Switches to plain names; @p reserved are names the dump refers to verbatim and hence must not pick.
    void plain(std::span<const std::string> reserved) { p_.plain(reserved); }

    /// Declares the annexes of the dumped file itself - Axm%s as `axm`s, Nom%s as `nom`s - grouped into the
    /// `mod` their names nest them in.
    void dump_annexes(std::string self, std::span<const DumpAnnex> annexes) {
        p_.self(std::move(self));
        auto content = [](const Def* def) { return def->isa<Axm>() ? def->type() : def->as<Nom>()->op(); };
        auto muts    = fe::Vector<Def*>();
        for (const auto& [_, __, annex] : annexes)
            for (auto mut : content(annex)->local_muts())
                if (isa_decl(mut)) muts.emplace_back(mut);
        dump_muts(muts);

        sep(Prev::Decl);
        auto block  = Block();
        auto names  = DefMap<std::string_view>();
        auto bodies = fe::Vector<Def*>();
        for (const auto& a : annexes) {
            block.emplace_back(a.def);
            names[a.def] = a.name;
            if (auto nom = a.def->isa<Nom>())
                for (auto mut : nom->local_muts())
                    bodies.emplace_back(mut);
        }
        if (!bodies.empty()) group_noms(Nest(bodies.front()->world(), bodies, true), block);

        auto item = block.begin();
        std::string_view curr_mod;
        for (const auto& [mod, name, def] : annexes) {
            if (mod != curr_mod) {
                if (!curr_mod.empty()) std::println(os_, "{}end", --p_.tab());
                if (!mod.empty()) std::println(os_, "{}mod {}", p_.tab(), mod), ++p_.tab();
                curr_mod = mod;
                p_.mod(mod.empty() ? std::string() : std::string(mod) + ".");
            }
            if (item == block.end() || item->def != def) continue; // prints with a later member of its group
            if (!item->group.empty())
                emit_mutual(item->group, [&](const Def* m) { emit_nom("anx nom", names[m], m->as<Nom>()); });
            else if (auto nom = def->isa<Nom>())
                emit_nom("anx nom", name, nom);
            else
                emit_axm(name, def->as<Axm>());
            ++item;
        }
        if (!curr_mod.empty()) std::println(os_, "{}end", --p_.tab());
        p_.mod({});
    }

    /// @name dump
    ///@{
    /// @p def and whatever Dumper::mode_ reaches from it.
    void dump(const Def* def) {
        if (auto mut = isa_decl(def)) return dump_muts({mut});

        auto muts = fe::Vector<Def*>();
        for (auto mut : def->local_muts())
            if (isa_decl(mut)) muts.emplace_back(mut);
        dump_muts(muts, def);
    }

    void dump(fe::View<Def*> muts) { dump_muts(muts); }
    ///@}

private:
    /// What the current block emitted last; only a declaration is worth setting apart.
    enum class Prev { None, Let, Decl };

    Plan& plan() { return p_.plan(); }
    const Plan& plan() const { return p_.plan(); }

    /// An `import` re-declares what it *exports* - but a plain `let` in that file stays local to it, so the dump has
    /// to spell that one out or nothing binds the name it refers to.
    bool is_imported(Def* mut) const { return mut->is_external() && srcs_.contains(mut->loc().src); }

    /// Schedules @p roots and emits what lands at the top level.
    void emit_top(const auto& roots) {
        for (auto root : roots)
            schedule_(root);
        emit(nullptr);
    }

    /// @name schedule
    ///@{
    /// The closed mutables @p muts reach, callees first: Mim binds a name before its uses.
    /// A non-decl @p tail ends the dump; it is spelled out after the `let`s it needs.
    void dump_muts(fe::View<Def*> muts, const Def* tail = nullptr) {
        if (muts.empty() && !tail) return;
        auto todo = fe::Vector<Def*>();
        for (auto mut : muts)
            post_order(mut, todo);
        for (auto [lam, match] : arm_of_)
            if (match && is_spellable(lam)) plan().arms.emplace(lam);

        // Nest::Node::topo_ breaks ties by gid, which a re-read dump need not reproduce.
        auto roots = fe::Vector<Def*>();
        for (auto mut : todo) {
            if (!mut->is_closed() || plan().arms.contains(mut)) continue;
            if (is_imported(mut))
                done_.emplace(mut);
            else
                roots.emplace_back(mut);
        }
        if (mode_ == Def::Dump::Scope) scope_.insert(muts.begin(), muts.end());

        const auto nest = Nest(tail ? tail->world() : muts.front()->world(), muts, true);
        auto _          = fe::Restore(nest_, &nest);
        emit_top(roots);
        if (tail) {
            emit_top(std::array{tail});
            sep(Prev::Let);
            emit_tail(tail, "");
        }
    }

    static bool is_spellable(const Def* arm) {
        auto lam = arm->isa_mut<Lam>();
        return lam && lam->is_set() && lam->filter() == lam->world().lit_tt();
    }

    /// Like mim::post_order, but visits the mutables in the order the operands reach them.
    /// Def::local_muts orders by pointer, which a re-read dump need not reproduce.
    void post_order(Def* mut, fe::Vector<Def*>& todo) {
        if (!scheduled_.emplace(mut).second) return;

        auto muts = fe::Vector<Def*>();
        auto seen = DefSet();
        printed_deps(mut, [&](const Def* op) {
            note_use(op, nullptr);
            local_muts(op, seen, muts);
        });
        for (auto local_mut : muts)
            post_order(local_mut, todo);

        todo.emplace_back(mut);
    }

    void local_muts(const Def* def, DefSet& seen, fe::Vector<Def*>& muts) {
        if (!def || def->local_muts().empty() || !seen.emplace(def).second) return;
        if (auto mut = def->isa_mut()) return (void)muts.emplace_back(mut);
        auto match = def->isa<Match>();
        printed_deps(def, [&](const Def* op) {
            // An arm twice in the same `match` cannot be spelled out at both places.
            auto arm = match && std::ranges::count(match->arms(), op) == 1;
            note_use(op, arm ? match : nullptr);
            local_muts(op, seen, muts);
        });
    }

    /// Records that @p op is an arm of @p match or, if `nullptr`, used otherwise.
    void note_use(const Def* op, const Match* match) {
        if (!op || !op->isa_mut<Lam>()) return;
        if (auto [i, ins] = arm_of_.emplace(op, match); !ins && !match) i->second = nullptr;
    }

    /// The Def%s the dump prints @p def with: a type nobody prints must not drag in a `let` that nothing refers to.
    /// A Lam spells out its Pi, so only the components of that one are referenced.
    void printed_deps(const Def* def, auto f) const {
        if (auto inst = p_.isa_inst(def)) {
            for (const auto& [arg, _] : inst->second)
                for (auto op : arg)
                    f(op);
            return;
        }
        if (auto lam = def->isa_mut<Lam>()) {
            f(lam->type()->dom());
            f(lam->type()->codom());
            for (auto op : lam->ops())
                f(op);
            return;
        }
        if (typed_let_ || def->isa<Lit>() || def->isa<Ext>() || def->isa<Inj>() || def->isa<Name>()) f(def->type());
        for (auto op : def->ops())
            f(op);
    }

    void note_vars(const Def* def) {
        if (auto inst = p_.isa_inst(def)) {
            for (const auto& [arg, _] : inst->second)
                for (auto op : arg)
                    if (op->isa<Var>()) plan().whole_vars.emplace(op);
            return;
        }
        auto ex = def->isa<Extract>();
        for (auto op : def->ops())
            if (op && op->isa<Var>() && !(ex && op == ex->tuple() && ex->index()->isa<Lit>()))
                plan().whole_vars.emplace(op);
    }

    /// @p let tells whether a `let` binds @p def, should it not print Full.
    void schedule_(const Def* def, bool let = true) {
        if (!def) return;
        if (auto mut = isa_decl(def); mut && !plan().arms.contains(mut)) return schedule_mut(mut);
        if (!done_.emplace(def).second) return finish(def);
        if (plan().arms.contains(def)) {
            // An arm has no block of its own either.
            auto lam = def->as_mut<Lam>();
            if (lam->body()->isa<Var>()) plan().whole_vars.emplace(lam->body());
            schedule_inline(lam->dom(), plan().inlined);
            schedule_inline(lam->body(), plan().inlined);
            return;
        }
        note_vars(def);
        schedule_deps(def);
        if ((let || p_.has_decl(def)) && !p_.is_full(def)) place({def});
    }

    void schedule_deps(const Def* def) {
        pending_.emplace(def);
        printed_deps(def, [&](const Def* op) { schedule_(op); });
        pending_.erase(def);
    }

    /// A mutable reached through @p def%'s deps may need @p def - and hence all of its deps - before they are done.
    void finish(const Def* def) {
        if (pending_.erase(def)) printed_deps(def, [&](const Def* op) { schedule_(op); });
    }

    void schedule_mut(Def* mut) {
        if (!enter(mut) || done_.contains(mut)) return;

        auto group = sibling_group(mut);
        for (auto m : group)
            done_.emplace(m);
        for (auto m : group)
            schedule_body(m);
        place({group.front(), group.size() > 1 ? DefVec(group.begin(), group.end()) : DefVec()});
    }

    /// A block only sees what precedes it, so the mutually recursive siblings of @p mut print as one `mutual` group.
    fe::Vector<Def*> sibling_group(Def* mut) const {
        auto node = (*nest_)[mut];
        if (!node || !node->is_mutually_recursive()) return {mut};
        if (auto key = owner(node->inest()); key && inlines(key)) return {mut};

        auto group = fe::Vector<Def*>();
        for (auto member : node->scc())
            if (auto m = member->mut(); m == mut || !done_.contains(m)) group.emplace_back(m);
        if (group.size() < 2 || !std::ranges::all_of(group, [this](Def* m) { return groupable(m); })) return {mut};
        return group;
    }

    bool groupable(Def* mut) const {
        if (!isa_decl(mut) || plan().arms.contains(mut) || mut->isa<Rule>() || !mut->is_set()) return false;
        if (auto lam = mut->isa_mut<Lam>()) return curry_chain(lam).back()->is_set();
        return true;
    }

    void schedule_body(Def* mut) {
        auto chain = mut->isa_mut<Lam>() ? curry_chain(mut->as_mut<Lam>()) : fe::Vector<Lam*>();

        if (auto rule = mut->isa_mut<Rule>()) {
            // The surface syntax has no block for a rule's parts, so whatever depends on its meta variable is inlined.
            printed_deps(rule->type(), [&](const Def* op) { schedule_inline(op, plan().inlined); });
            printed_deps(rule, [&](const Def* op) { schedule_inline(op, plan().inlined); });
        } else if (chain.empty()) {
            for (auto op : mut->deps())
                schedule_(op);
        } else {
            for (auto* lam : chain)
                if (lam != mut) done_.emplace(lam), absorbed_.emplace(lam, mut);
            // The chain spells out its Pi%s, so only their components are referenced.
            for (auto* lam : chain) {
                schedule_inline(lam->type()->dom(), plan().header);
                if (auto var = lam->has_var()) schedule_proj_types(var);
                schedule_inline(lam->filter(), plan().header);
                if (lam == chain.back()) {
                    schedule_inline(lam->type()->codom(), plan().header);
                    schedule_tail(lam->body());
                }
            }
        }
    }

    /// A destructured parameter prints the types of its projections, where the Lam's own Var is already substituted.
    void schedule_proj_types(const Def* def) {
        auto n = def->num_tprojs();
        if (n <= 1) return;
        for (size_t i = 0; i != n; ++i)
            if (auto proj = def->proj(n, i)) {
                schedule_inline(proj->type(), plan().header);
                schedule_proj_types(proj);
            }
    }

    /// Schedules what @p def needs; @p def itself, noted in @p inline_set, prints inline.
    void schedule_inline(const Def* def, DefSet& inline_set) {
        if (!def || def->is_closed() || p_.has_decl(def)) return schedule_(def);
        if (!inline_set.emplace(def).second) return;
        note_vars(def);
        printed_deps(def, [&](const Def* op) { schedule_inline(op, inline_set); });
    }

    /// A Lam's body is the tail of its block, so it is emitted there instead of as a `let` of its own.
    void schedule_tail(const Def* def) {
        if (def && def->isa<Var>()) plan().whole_vars.emplace(def);
        schedule_(def, false);
    }

    /// Is @p mut ours to emit - or does it only get referenced by name?
    /// Dump::Scope only enters what nests in one of Dumper::scope_.
    bool enter(Def* mut) const {
        auto node = (*nest_)[mut];
        if (!node || mode_ != Def::Dump::Scope) return node != nullptr;
        while (!node->inest()->is_root())
            node = node->inest();
        return scope_.contains(node->mut());
    }

    /// Appends @p item to the block it belongs into.
    void place(Item item) {
        auto def = item.def;
        auto mut = isa_decl(def);
        auto key = owner(nest_->inest(def));
        // A Nom is the one immutable with a declaration of its own, so it never inlines.
        if (def->isa<Nom>()) {
            while (key && inlines(key))
                key = owner((*nest_)[key]->inest());
            // TODO Without its generic `nom`, an instance over a Lam's Var has no valid place in that Lam's
            // signature. See lit/nom_generic_roundtrip.mim.
        }
        // A binder that prints inline has no block of its own, so what belongs into it is inlined as well.
        if (key && inlines(key) && !(mut && is_recursive(mut))) return (void)plan().inlined.emplace(def);
        (key ? blocks_[key] : top_).emplace_back(std::move(item));
    }

    /// Does @p mut print inline - be it one itself, or because it sits inside one that does?
    bool inlines(Def* mut) const {
        for (auto node = (*nest_)[mut]; node; node = node->inest())
            if (auto m = owner(node); m && (!isa_decl(m) || plan().arms.contains(m))) return true;
        return false;
    }

    bool is_recursive(Def* mut) const { return (*nest_)[mut]->is_recursive(); }
    /// The mutable whose block @p node stands for; `nullptr` for the top level.
    Def* owner(const Nest::Node* node) const {
        if (!node) return nullptr;
        auto mut = node->mut();
        if (!mut) return nullptr;                                  // a *virtual* root is the top level
        if (auto outer = fe::lookup(absorbed_, mut)) return outer; // a curried chain shares a block
        return mut;
    }

    ///@}

    /// @name emit
    ///@{
    void emit(Def* key) {
        auto block = std::exchange(key ? blocks_[key] : top_, {});
        group_noms(*nest_, block);
        for (const auto& [def, group] : block) {
            sep(!group.empty() || isa_decl(def) ? Prev::Decl : Prev::Let);
            if (group.empty())
                emit_item(def);
            else
                emit_mutual(group, [this](const Def* m) { emit_item(m); });
        }
    }

    void emit_item(const Def* def) {
        if (auto mut = isa_decl(def)) return emit_decl(mut);
        emit_let(def);
    }

    void emit_axm(std::string_view name, const Axm* axm) {
        std::print(os_, "{}axm {}: {}", p_.tab(), name, p_.op(axm->type()));
        auto [curry, trip] = Axm::infer_curry_and_trip(axm->type());
        if (axm->curry() != curry || axm->trip() != trip) std::print(os_, ", {}", axm->curry());
        if (axm->trip() != trip) std::print(os_, ", {}", axm->trip());
        std::println(os_, ";");
    }

    void emit_nom(std::string_view kw, std::string_view name, const Nom* nom) {
        std::println(os_, "{}{} {} = {};", p_.tab(), kw, name, p_.rhs(p_.op(nom->op())));
    }

    /// Groups the Nom%s of @p block that lie on a cycle; the group takes the place of its last member.
    /// No immutable refers back to itself, so such a cycle runs through a recursive SCC.
    /// Everything a cycle needs precedes its last member.
    void group_noms(const Nest& nest, Block& block) {
        for (size_t i = 0; i < block.size(); ++i) {
            auto nom = block[i].def->isa<Nom>();
            if (!nom || !block[i].group.empty()) continue;
            for (auto mut : nom->local_muts()) {
                auto node = nest[mut];
                if (!node || !node->is_recursive()) continue;
                auto on_cycle = scc_noms(nest, node->scc());
                auto cycle    = DefVec();
                for (const auto& item : block)
                    if (on_cycle.contains(item.def)) cycle.emplace_back(item.def);
                if (cycle.size() > 1) {
                    i = size_t(-1); // start over: the block shrinks
                    std::erase_if(block, [&](const Item& item) {
                        return item.def != cycle.back() && on_cycle.contains(item.def);
                    });
                    std::ranges::find(block, cycle.back(), &Item::def)->group = std::move(cycle);
                }
                break;
            }
        }
    }

    /// The Nom%s @p scc reaches that reach it back.
    static DefSet scc_noms(const Nest& nest, const Nest::Node::SCC& scc) {
        auto reaches_scc = [&](const Def* nom) {
            return std::ranges::any_of(nom->local_muts(), [&](Def* mut) { return scc.contains(nest[mut]); });
        };
        auto res  = DefSet();
        auto todo = DefVec();
        auto seen = DefSet();
        for (auto node : scc)
            for (auto dep : node->mut()->deps())
                if (dep && seen.emplace(dep).second) todo.emplace_back(dep);
        while (!todo.empty()) {
            auto def = todo.back();
            todo.pop_back();
            if (def->isa_mut()) continue;
            if (def->isa<Nom>() && reaches_scc(def)) res.emplace(def);
            for (auto dep : def->deps())
                if (dep && seen.emplace(dep).second) todo.emplace_back(dep);
        }
        return res;
    }

    void emit_mutual(const DefVec& group, auto emit_member) {
        std::println(os_, "{}mutual", p_.tab());
        ++p_.tab();
        {
            auto _ = fe::Restore(prev_, Prev::None);
            for (auto def : group) {
                sep(isa_decl(def) ? Prev::Decl : Prev::Let);
                emit_member(def);
            }
        }
        std::println(os_, "{}end", --p_.tab());
    }

    /// A declaration spans several lines, so a blank line sets it apart from its neighbors within the same block.
    void sep(Prev curr) {
        if (prev_ == Prev::Decl || (prev_ == Prev::Let && curr == Prev::Decl)) os_ << '\n';
        prev_ = curr;
    }

    void emit_let(const Def* def) {
        if (auto nom = def->isa<Nom>()) return emit_nom("nom", p_.name(nom), nom);
        std::print(os_, "{}let {}", p_.tab(), p_.name(def));
        if (typed_let_) std::print(os_, ": {}", p_.op(def->type()));
        std::println(os_, " = {};", p_.full(def));
    }

    void emit_decl(Def* mut) {
        if (auto lam = mut->isa_mut<Lam>()) return emit_lam(lam);
        if (auto rule = mut->isa_mut<Rule>(); rule && rule->is_set()) return emit_rule(rule);
        if (!mut->is_set()) return emit_unset(mut);
        // `rec` binds the name for the body - which only a self-referential mutable needs; `extern` is out either way.
        std::println(os_, "{}{}{} = {};", p_.tab(), is_recursive(mut) ? "rec " : "let ", p_.id(mut),
                     p_.rhs(p_.full(mut)));
    }

    void emit_rule(Rule* rule) {
        std::print(os_, "{}rule {} {}: {}", p_.tab(), p_.id(rule), p_.params(rule), p_.full(rule->lhs()));
        if (rule->guard() != rule->world().lit_tt()) std::print(os_, " when {}", p_.full(rule->guard()));
        std::println(os_, " => {};", p_.full(rule->rhs()));
    }

    /// Nothing declares a mutable that was never set, so leave a trace instead of an unreadable dump.
    void emit_unset(Def* mut) { std::println(os_, "{}// `{}: {}` is unset", p_.tab(), p_.id(mut), p_.op(mut->type())); }

    void emit_lam(Lam* lam) {
        auto chain = curry_chain(lam);
        auto last  = chain.back();
        auto kind  = lam_kind(last, p_.isa_fun(last) && !shadows_ret(last));

        // A `fun` binds its `ret` continuation as `return`, so that is the name its Var has to print with.
        if (kind == LamKind::Fun)
            if (auto var = last->has_var()) p_.ret(var);
        // A Lam without a body forward-declares what a native translation unit provides - and only an `extern`
        // may go without one.
        if (!last->is_set()) return emit_bodyless(lam, last, kind);

        auto name = p_.id(lam);
        std::print(os_, "{}{}{} {}", p_.tab(), lam->is_external() ? "extern " : "", decl_kw(kind), name);
        p_.lam_header(os_, chain, kind);
        os_ << " =\n";

        // An inner Lam of the chain has no name; its outer Lam%s applied to their own Var%s reduce to it.
        auto ref = name;
        for (size_t i = 1; i != chain.size(); ++i) {
            auto var = chain[i - 1]->has_var();
            if (!var) break;
            ref += std::format(" {}{}", chain[i - 1]->type()->is_implicit() ? "@" : "", p_.name(var));
            p_.bind(chain[i], std::format("({})", ref));
        }

        ++p_.tab();
        rets_.emplace_back(kind == LamKind::Fun ? last->has_var() : nullptr);
        auto _ = fe::Restore(prev_, Prev::None);
        emit(lam);
        sep(Prev::Let);
        emit_tail(last->body(), ";");
        rets_.pop_back();
        --p_.tab();
    }

    /// A Lam without a body only ever declares what some backend provides: `extern con f [mem.M 0, I32];`.
    void emit_bodyless(Lam* lam, Lam* last, LamKind kind) {
        auto dom = last->type()->dom();
        std::print(os_, "{}extern {} {}", p_.tab(), decl_kw(kind), p_.id(lam));
        auto num = num_binders(dom);
        if (kind == LamKind::Fun)
            return std::println(os_, "[{}]: {};", p_.op(dom->proj(num, 0)), p_.op(last->ret_dom()));

        // Flattened, the components of a dependent domain would refer to binders nothing prints.
        if (auto [_, var] = dom->isa_binder<Sigma>(); var)
            os_ << p_.op(dom);
        else
            std::print(os_, "[{}]", p_.ops(dom->projs(num)));
        if (kind == LamKind::Lam) std::print(os_, ": {}", p_.op(last->type()->codom()));
        os_ << ";\n";
    }

    void emit_tail(const Def* def, std::string_view end) { std::println(os_, "{}{}{}", p_.tab(), p_.tail(def), end); }

    /// Would a `fun` here hide the `return` of an enclosing one @p lam might still refer to?
    /// The `return` is a projection of its Lam's Var, so capturing that Var at all is as precise as this gets.
    bool shadows_ret(Lam* lam) const {
        for (auto* var : rets_)
            if (var && lam->has_free_var(var)) return true;
        return false;
    }

    ///@}

    std::ostream& os_;
    Def::Dump mode_;
    bool typed_let_;
    const Nest* nest_ = nullptr; ///< Nests the mutables of the scope being emitted.
    Prev prev_        = Prev::None;
    fe::Vector<const Var*> rets_;
    Block top_;             ///< What the top level declares, in dependency order.
    MutMap<Block> blocks_;  ///< Likewise for the block of each mutable that has one.
    MutMap<Def*> absorbed_; ///< Inner Lam of a curried chain -> the chain's outermost one.
    DefSet done_;
    DefSet scope_;   ///< The closed mutables Dump::Scope dumps.
    DefSet pending_; ///< Def%s whose deps are being scheduled.
    Printer p_;
    Srcs srcs_;
    MutSet scheduled_;
    /// A `match` a Lam is an arm of, or `nullptr` if it is used otherwise and hence needs a declaration.
    /// Re-reading spells out an arm per `match` anyway, so one shared by several is spelled out in each.
    DefMap<const Match*> arm_of_;
};

/// The name of the file a World is dumped to - and hence the module its own annexes live in.
std::string file_stem(World& world) { return (world.name() ? world.name() : world.sym("_default")).str(); }

/// Spells out the `import`s again; yields their files and adds the modules they name to @p reserved.
Srcs dump_imports(std::ostream& os, const Driver& driver, fe::Vector<std::string>& reserved) {
    auto srcs = Srcs();
    for (const auto& import : driver.imports()) {
        srcs.emplace(import.src);
        auto kw     = import.tag == ast::Tok::Tag::K_plugin ? "plugin" : "import";
        auto module = import.src->path().stem().string();
        // A stem that is no identifier names no module, so the file has to be spliced instead.
        auto is_mod = ast::Lexer::is_id(module);
        if (is_mod) reserved.emplace_back(std::move(module));
        // The spelling was relative to the importing file; only the resolved path re-parses from here.
        // Generic format: a native Windows `\` would lex as an escape sequence inside the string literal.
        if (import.path)
            std::print(os, "{} \"{}\"{};\n", kw, ast::Lexer::escape(import.src->path().generic_string()),
                       is_mod ? "" : " as *");
        else
            std::print(os, "{} {};\n", kw, import.sym);
    }
    return srcs;
}

/// The generic `anx nom`s from elsewhere - the Nom%s the annex Lam%s of an `import` return - by their Nom::key.
/// An instance shares that key; @p self is the `<world name>.` prefix of the annexes the dump declares itself.
auto find_generics(World& world, std::string_view self) {
    auto res = ankerl::unordered_dense::map<flags_t, std::pair<Sym, fe::Vector<Lam*>>>();
    auto lib = world.driver().library();
    for (auto w : {&world, lib == &world ? nullptr : lib})
        if (w)
            for (auto [sym, def] : w->annexes().entries())
                if (auto lam = def->isa_mut<Lam>(); lam && !sym.view().starts_with(self)) {
                    auto chain = curry_chain(lam);
                    if (auto body = chain.back()->body())
                        if (auto nom = body->isa<Nom>()) res.emplace(nom->key(), std::pair{sym, std::move(chain)});
                }
    return res;
}

/// What the externals reach.
struct Reached {
    DefSet live;
    fe::Vector<DumpAnnex> annexes; ///< The annexes of the dumped file itself, in declaration order.
    Insts insts;
};

/// An `import` declares its own annexes; those of the dumped file itself only exist if the dump declares them.
/// Only a loaded plugin registers its annexes, so find them by what the externals reach.
Reached reach(World& world, std::string_view self) {
    auto generics = find_generics(world, self);
    auto res      = Reached();
    auto todo     = DefVec(world.externals().muts().begin(), world.externals().muts().end());
    res.live.insert(todo.begin(), todo.end());
    while (!todo.empty()) {
        auto def = todo.back();
        todo.pop_back();
        if ((def->isa<Axm>() || is_anx_nom(def)) && def->sym().view().starts_with(self)) {
            auto sym  = def->sym(); // a short Sym is stored inline, so its view must not outlive it
            auto name = sym.view().substr(self.size());
            auto dot  = name.find('.');
            auto mod  = dot == std::string_view::npos ? std::string_view() : name.substr(0, dot);
            res.annexes.emplace_back(std::string(mod), std::string(mod.empty() ? name : name.substr(dot + 1)), def);
        }
        if (auto nom = def->isa<Nom>())
            if (auto generic = fe::lookup(generics, nom->key()))
                if (auto args = Instantiation(generic->second).args(nom))
                    res.insts.emplace(nom, std::pair{generic->first, std::move(*args)});
        for (auto dep : def->deps())
            if (dep && res.live.emplace(dep).second) todo.emplace_back(dep);
    }
    std::ranges::sort(res.annexes, {}, [](const auto& a) { return a.def->flags(); });
    return res;
}

} // namespace

/*
 * Def
 */

/// Def::Dump::Expr: one Def, one line, no analysis - and the backend of `std::formatter` for every Def pointer.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    auto _       = def->world().freeze();
    auto printer = Printer(true);
    return os << printer.op(def);
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
    auto file = Printer(true).id(this) + ".mim"s;
    write(mode, file.c_str());
}

/*
 * World
 */

void World::dump(std::ostream& os) {
    auto _        = freeze();
    auto old_gid  = curr_gid();
    auto reserved = fe::Vector<std::string>{"return"};
    auto srcs     = dump_imports(os, driver(), reserved);

    if (!driver().imports().entries().empty() && externals().size() != 0) os << '\n';

    for (auto mut : externals().muts())
        reserved.emplace_back(mut->sym().str());

    auto self                   = file_stem(*this) + ".";
    auto [live, annexes, insts] = reach(*this, self);
    for (const auto& annex : annexes)
        reserved.emplace_back(annex.mod.empty() ? annex.name : annex.mod);

    auto dumper = Dumper(os, Def::Dump::All, flags().mim_typed_let, std::move(srcs));
    dumper.plain(reserved);
    dumper.live(live);
    dumper.insts(std::move(insts));
    if (!annexes.empty()) dumper.dump_annexes(std::move(self), annexes);
    dumper.dump(externals().mutate());

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
    auto file = file_stem(*this) + ".mim"s;
    write(file.c_str());
}

} // namespace mim
