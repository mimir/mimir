#include <fstream>
#include <ostream>
#include <ranges>

#include <fe/assert.h>
#include <fe/worklist.h>

#include "mim/driver.h"

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

/// Names the running dump binds itself: a `fun` calls its `ret` continuation `return`, whatever its Def::sym.
/// Keyed by Var and index, because the projection itself need not exist as a Def while the World is frozen.
struct Renames {
    DefMap<std::string> defs;
    DefMap<nat_t> ret_projs; ///< Var -> index of the `ret` component within it.
    DefSet opaque;           ///< Def%s no pattern took apart, so their components have no name of their own.
};

thread_local Renames* renames = nullptr;

/// Def::unique_name - or the plain Def::sym while a diagnostic is being formatted, where a gid is noise.
std::string name(const Def* def) {
    if (renames) {
        if (auto i = renames->defs.find(def); i != renames->defs.end()) return i->second;
        if (auto ex = def->isa<Extract>())
            if (auto i = renames->ret_projs.find(ex->tuple()); i != renames->ret_projs.end())
                if (Lit::isa(ex->index()) == i->second) return "return"s;
    }
    if (auto sym = def->sym(); sym && sym != '_' && PlainNames::claim(def->world().driver(), sym, def->gid()))
        return sym.str();
    return def->unique_name();
}

std::string id(const Def* def) {
    if (def->is_external() || (!def->is_set() && def->isa<Lam>())) return def->sym().str();
    return name(def);
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
    if (def->isa<Inj>()) return Prec::Inj;
    if (def->isa<Reform>()) return Prec::App;
    // `Cn e` parses its domain at Prec::Bot, so it swallows whatever follows and only ever fits a closed context.
    if (auto pi = def->isa<Pi>()) return Pi::isa_cn(pi) ? Prec::Bot : Prec::Arrow;
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
    Op(const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : def_(def)
        , prec_(prec)
        , is_left_(is_left) {}
    static Op l(const Def* def, Prec prec = Prec::Bot) { return {def, prec, true}; }
    static Op r(const Def* def, Prec prec = Prec::Bot) { return {def, prec, false}; }

    static auto map(const auto& range, const char* sep = ", ", Prec prec = Prec::Bot) {
        return fe::Join(range | std::views::transform([prec](auto op) { return Op(op, prec); }), sep);
    }

    /// @name Getters
    ///@{
    Prec prec() const { return prec_; }
    bool is_left() const { return is_left_; }
    const Def* def() const { return def_; }
    const Def* operator->() const { return def_; }
    const Def* operator*() const { return def_; }
    explicit operator bool() const { return def_ != nullptr; }
    ///@}

private:
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
    Full(const Def* def, Prec prec = Prec::Bot, bool is_left = false)
        : Op(def, prec, is_left) {}
    Full(Op op)
        : Full(op.def(), op.prec(), op.is_left()) {}

    explicit operator bool() const { return is_inline(); }

    bool is_inline() const {
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
std::string shape(const Seq* seq) {
    auto s = seq->shape();
    auto r = s.rank();
    if (!r || *r <= 1) return std::format("{}", Op(*s));

    auto axes = s->projs(*r);
    if (std::ranges::any_of(axes, [](auto a) { return !a; })) return std::format("{}", Op(*s));
    return std::format("{}", Op::map(axes));
}

/// As above but each axis binds its index - `i: 2, j: 3`; a fused Seq binds one Var per axis.
std::string shape(const Seq* seq, const Def* var) {
    auto s = seq->shape();
    auto r = s.rank();
    if (!r || *r <= 1) return std::format("{}: {}", Op(var), Op(*s));

    auto res = std::string();
    for (auto sep = ""; auto i : std::views::iota(nat_t(0), *r)) {
        auto axis   = var->proj(*r, i);
        auto extent = s[i];
        if (!extent) return std::format("{}: {}", Op(var), Op(*s));
        // A projection the body never mentions does not exist as a Def and hence has no name of its own.
        res += std::format("{}{}: {}", sep, axis ? std::format("{}", Op(axis)) : "_"s, Op(extent));
        sep = ", ";
    }
    return res;
}

/// May @p def of @p type be taken apart into @p n components?
/// A component the frozen World does not hand out has no name, so it has to fall back to @p type - and that
/// spells a *dependent* component with the domain's binders instead of this pattern's. That is where
/// destructuring stops: @p def stays opaque and its components print as `def#i` rather than by a dangling name.
bool destructible(const Def* def, const Def* type, size_t n) {
    if (!def || n <= 1) return false;
    auto var = type->isa_mut<Sigma>() ? type->has_var() : nullptr;

    for (size_t i = 0; i != n; ++i) {
        if (def->proj(n, i)) continue; // it exists and hence brings its own name and type
        auto t = type->proj(n, i);
        if (!t || (var && t->has_free_var(var))) {
            if (renames) renames->opaque.emplace(def);
            return false;
        }
    }
    return true;
}

/// @p brckt selects the `[]` sub-patterns of a type over the `()` sub-patterns of a Lam's parameter list.
void ptrn(std::ostream& os, const Def* def, const Def* type, bool brckt = false) {
    if (!def) return std::print(os, "_: {}", Op(type));

    auto n = def->num_tprojs();
    if (!destructible(def, type, n)) return std::print(os, "{}: {}", name(def), Op(type));

    os << (brckt ? '[' : '(');
    for (auto sep = ""; auto i : std::views::iota(size_t(0), n)) {
        auto proj = def->proj(n, i);
        os << sep;
        // A projection's own type is the one where the binder has already been substituted for this Var.
        ptrn(os, proj, proj ? proj->type() : type->proj(n, i), brckt);
        sep = ", ";
    }
    // The alias keeps a name for the entity as a whole - which the body may well refer to.
    std::print(os, "{} as {}", brckt ? ']' : ')', name(def));
}

/// A Pi's domain the way the surface syntax binds it - a pattern, once the Var's projections are in use.
void dom_ptrn(std::ostream& os, const Def* var, const Def* type, bool implicit) {
    auto l = implicit ? '{' : '[';
    auto r = implicit ? '}' : ']';
    auto n = var->num_tprojs();
    if (!destructible(var, type, n)) return std::print(os, "{}{}: {}{}", l, name(var), Op(type), r);

    os << l;
    for (auto sep = ""; auto i : std::views::iota(size_t(0), n)) {
        os << sep;
        auto proj = var->proj(n, i);
        ptrn(os, proj, proj ? proj->type() : type->proj(n, i), true);
        sep = ", ";
    }
    std::print(os, "{} as {}", r, name(var));
}

void bndr(std::ostream& os, const Def* def, const Def* type) {
    if (def) return ptrn(os, def, def->type());
    std::print(os, "_: {}", Op(type));
}

/// @p num is how many binders the domain has; @p limit how many of them to print - a returning Lam hides its last.
void curry(std::ostream& os, const Def* def, const Def* type, bool implicit, size_t num, size_t limit, bool alias) {
    auto l = implicit ? '{' : '(';
    auto r = implicit ? '}' : ')';

    if (limit == 0) return (void)(os << l << r);
    // Same as in ptrn: a parameter list only comes apart if every parameter is there to be named.
    if (limit == num && !destructible(def, type, num)) {
        if (def) return std::print(os, "{}{}: {}{}", l, name(def), Op(type), r);
        return std::print(os, "{}_: {}{}", l, Op(type), r);
    }

    os << l;
    for (auto sep = ""; auto i : std::views::iota(size_t(0), limit)) {
        os << sep;
        auto proj = def ? def->proj(num, i) : nullptr;
        bndr(os, proj, proj ? proj->type() : type->proj(num, i));
        sep = ", ";
    }
    os << r;
    if (alias && def) std::print(os, " as {}", name(def));
}

std::ostream& operator<<(std::ostream& os, Op op) {
    if (*op == nullptr) return os << "<nullptr>";
    if (auto d = Full(op)) return os << d;
    return os << id(*op);
}

std::ostream& operator<<(std::ostream& os, Full d) {
    if (auto hole = d->isa_mut<Hole>()) return hole->is_set() ? os << Op(hole->op()) : os << "?";
    if (auto mut = d->isa_mut(); mut && !mut->is_set()) return os << "unset";
    if (d.needs_parens()) return os << std::format("({})", Full(*d));

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
            if (level == 0) return os << "*";
            if (level == 1) return os << "□";
        }
        return os << std::format("Type {}", Op::r(type->level(), Prec::App));
    } else if (auto reform = d->isa<Reform>()) {
        return os << std::format("Rule {}", Op::r(reform->dom(), Prec::App));
    } else if (d->isa<Univ>()) {
        return os << "Univ";
    } else if (d->isa<Nat>()) {
        return os << "Nat";
    } else if (d->isa<Idx>()) {
        return os << "Idx";
    } else if (auto ext = d->isa<Ext>()) {
        return os << std::format("{}:{}", ext->isa<Bot>() ? bot : top, Op::r(ext->type(), Prec::Lit));
    } else if (auto axm = d->isa<Axm>()) {
        return os << axm->sym();
    } else if (auto lit = d->isa<Lit>()) {
        if (lit->type()->isa<Nat>()) {
            // clang-format off
            switch (lit->get()) {
                case 0x0'0000'0100_n: return os << "i8";
                case 0x0'0001'0000_n: return os << "i16";
                case 0x1'0000'0000_n: return os << "i32";
                default: return os << std::format("{}", lit->get());
            }
            // clang-format on
        } else if (auto size = Idx::isa(lit->type())) {
            if (auto s = Lit::isa(size)) {
                // clang-format off
                switch (*s) {
                    case 0x0'0000'0002_n: return os << (lit->get<bool>() ? "tt" : "ff");
                    case 0x0'0000'0100_n: return os << lit->get() << "I8";
                    case 0x0'0001'0000_n: return os << lit->get() << "I16";
                    case 0x1'0000'0000_n: return os << lit->get() << "I32";
                    case             0_n: return os << lit->get() << "I64";
                    default: {
                        os << lit->get();
                        std::vector<uint8_t> digits;
                        for (auto z = *s; z; z /= 10) digits.emplace_back(z % 10);

                        if (ascii) {
                            os << '_';
                            for (auto d : digits | std::views::reverse)
                                os << char('0' + d);
                        } else {
                            for (auto d : digits | std::views::reverse)
                                os << uint8_t(0xE2) << uint8_t(0x82) << (uint8_t(0x80 + d));
                        }
                        return os;
                    }
                }
                // clang-format on
            }
        }
        return os << std::format("{}:{}", lit->get(), Op::r(lit->type(), Prec::Lit));
    } else if (auto ex = d->isa<Extract>()) {
        // A Var's component prints by name - unless the pattern that would have bound that name kept the Var whole.
        auto opaque = renames && renames->opaque.contains(ex->tuple());
        if (!opaque && ex->tuple()->isa<Var>() && ex->index()->isa<Lit>()) return os << name(ex);
        return os << std::format("{}#{}", Op::l(ex->tuple(), Prec::Extract), Op::r(ex->index(), Prec::Extract));
    } else if (auto ins = d->isa<Insert>()) {
        auto tup = Op::l(ins->tuple(), Prec::Extract);
        // `←` updates the whole `#`-path, so an Extract target needs parens to re-parse.
        if (auto ex = ins->tuple()->isa<Extract>(); ex && !(ex->tuple()->isa<Var>() && ex->index()->isa<Lit>()))
            os << std::format("({})", tup);
        else
            os << std::format("{}", tup);
        return os << std::format("#{} ← {}", Op::r(ins->index(), Prec::Extract), Op::r(ins->value(), Prec::Ins));
    } else if (auto var = d->isa<Var>()) {
        return os << name(var);
    } else if (auto [pi, var] = d->isa_binder<Pi>(); pi) {
        dom_ptrn(os, var, pi->dom(), pi->is_implicit());
        std::print(os, " {} {}", arw, Op::r(pi->codom(), Prec::Arrow));
        return os;
    } else if (auto pi = d->isa<Pi>()) {
        if (Pi::isa_cn(pi)) return os << std::format("Cn {}", Op(pi->dom()));
        if (pi->is_implicit())
            return os << std::format("{{_: {}}} {} {}", Op(pi->dom()), arw, Op::r(pi->codom(), Prec::Arrow));
        return os << std::format("{} {} {}", Op::l(pi->dom(), Prec::Arrow), arw, Op::r(pi->codom(), Prec::Arrow));
    } else if (auto lam = d->isa<Lam>()) {
        // TODO this output is really confuinsg
        return os << std::format("{}, {}", Op(lam->filter()), Op(lam->body()));
    } else if (auto app = d->isa<App>()) {
        if (auto size = Idx::isa(app)) {
            if (auto l = Lit::isa(size)) {
                // clang-format off
                switch (*l) {
                    case 0x0'0000'0002_n: return os << "Bool";
                    case 0x0'0000'0100_n: return os << "I8";
                    case 0x0'0001'0000_n: return os << "I16";
                    case 0x1'0000'0000_n: return os << "I32";
                    case             0_n: return os << "I64";
                    default: break;
                }
                // clang-format on
            }
        }

        if (Pi::isa_implicit(app->callee()->unfold_type())) {
            // An argument that is still a Hole is what the parser inserts anyway - and `?` has no spelling.
            if (app->arg()->has_dep(Dep::Hole)) return os << Op(app->callee(), d.prec(), d.is_left());
            // Spelling out an implicit argument needs `@`; juxtaposition would insert a Hole in front of it.
            return os << std::format("{} @ {}", Op::l(app->callee(), Prec::App), Op::r(app->arg(), Prec::App));
        }
        return os << std::format("{} {}", Op::l(app->callee(), Prec::App), Op::r(app->arg(), Prec::App));
    } else if (auto [sigma, var] = d->isa_binder<Sigma>(); sigma) {
        size_t i  = 0;
        auto elem = fe::StreamFn{[&](std::ostream& os) -> std::ostream& {
            auto sep = "";
            for (auto op : sigma->ops()) {
                os << sep;
                if (auto v = sigma->var(sigma->num_ops(), i++))
                    os << std::format("{}: {}", v, Op(op));
                else
                    os << Op(op);
                sep = ", ";
            }
            return os;
        }};

        return os << std::format("[{}]", elem);
    } else if (auto sigma = d->isa<Sigma>()) {
        return os << std::format("[{}]", Op::map(sigma->ops()));
    } else if (auto tuple = d->isa<Tuple>()) {
        return os << std::format("({})", Op::map(tuple->ops()));
    } else if (auto [arr, var] = d->isa_binder<Arr>(); arr) {
        return os << std::format("{}{}; {}{}", al, shape(arr, var), Op(arr->body()), ar);
    } else if (auto arr = d->isa<Arr>()) {
        return os << std::format("{}{}; {}{}", al, shape(arr), Op(arr->body()), ar);
    } else if (auto [pack, var] = d->isa_binder<Pack>(); pack) {
        return os << std::format("{}{}; {}{}", pl, shape(pack, var), Op(pack->body()), pr);
    } else if (auto pack = d->isa<Pack>()) {
        return os << std::format("{}{}; {}{}", pl, shape(pack), Op(pack->body()), pr);
    } else if (auto proxy = d->isa<Proxy>()) {
        return os << std::format("(proxy#{} {})", proxy->tag(), Op::map(proxy->ops()));
    } else if (auto bound = d->isa<Bound>()) {
        auto op = bound->isa<Join>() ? "∪" : "∩"; // TODO ascii
        if (auto mut = d->isa_mut()) std::print(os, "{}{}: {}", op, name(mut), Op(mut->type()));
        if (!bound->isa<Join>()) return os << std::format("{}({})", op, Op::map(bound->ops()));
        return os << Op::map(bound->ops(), " ∪ ", Prec::Union);
    } else if (auto inj = d->isa<Inj>()) {
        return os << std::format("{} inj {}", Op::l(inj->value(), Prec::Inj), Op::r(inj->type(), Prec::Inj));
    } else if (auto uniq = d->isa<Uniq>()) {
        return os << std::format("⦃{}⦄", Op(uniq->op())); // TODO ascii
    }

    // other
    auto tag = d->flags() == 0 ? std::string(d->node_name()) : std::format("{}#{}", d->node_name(), d->flags());
    if (d->ops().empty()) return os << std::format("({})", tag);
    return os << std::format("({} {})", tag, Op::map(d->ops(), " "));
}

/*
 * Dumper
 */

/// Is @p lam sugar for a `fun`?
/// That sugar folds the domain into `[<params>, Cn <ret>]`, so a *flat* domain with a trailing Cn stays a `con`.
bool isa_fun(const Lam* lam) {
    auto pi = lam->type();
    return Lam::isa_returning(lam) && num_binders(pi->dom()) == 2 && Pi::isa_basicblock(pi->dom(2, 1));
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
/// A Def belongs into the innermost binder whose Var it mentions - and to the top level, if it mentions none.
/// Free Var%s are all it takes to place it: no Nest, no dominance, no fixed point.
/// That matters because Def::dump is what you reach for in a debugger, where the World is usually half-built:
/// every analysis a dump runs is one more way for it to die on the very program you want to look at.
class Dumper {
public:
    Dumper(std::ostream& os, Dump mode)
        : os_(os)
        , mode_(mode) {}

    /// @name schedule
    ///@{
    /// Assigns @p def - and whatever Dumper::mode_ reaches from it - to the scope it belongs to.
    void schedule(const Def* def) {
        roots_.emplace(def);
        done_.erase(def);
        schedule_(def);
    }
    ///@}

    /// @name emit
    ///@{
    void emit_top() {
        auto _ = fe::Restore(renames, &renames_);
        emit(nullptr);
    }
    void emit_expr(const Def* def) { emit_tail(def, ""); }
    /// Dump::Local hands the mutables it did not enter back, so a caller can dump them as roots of their own.
    void drain() {
        while (!muts_.empty()) {
            schedule(muts_.pop());
            emit_top();
        }
    }
    ///@}

private:
    /// One open binder: @p key is the mutable whose body absorbs it - a curried Lam chain shares one.
    struct Frame {
        Def* key;
        Vars vars;
    };

    static constexpr size_t Top = size_t(-1);

    /// @name schedule
    ///@{
    void schedule_(const Def* def) {
        if (!def) return;
        if (auto mut = isa_decl(def); mut && open_.contains(mut)) recursive_.emplace(mut);
        if (!done_.emplace(def).second) return;
        if (auto mut = isa_decl(def)) return schedule_mut(mut);
        for (auto op : def->deps())
            schedule_(op);
        if (!Full(def)) place_here(def);
    }

    /// Places @p def - unless its Var%s belong to a scope this path is not inside of; then it waits for one that is.
    void place_here(const Def* def) {
        if (auto lvl = level(def); lvl != Top || !def->has_free_vars())
            place(def, lvl);
        else
            done_.erase(def);
    }

    void schedule_mut(Def* mut) {
        auto lvl = level(mut);
        if (lvl == Top && mut->has_free_vars()) return (void)done_.erase(mut); // not this path's scope
        if (!enter(mut, lvl)) {
            if (mode_ == Dump::Local) muts_.push(mut);
            return;
        }

        auto chain = mut->isa_mut<Lam>() ? curry_chain(mut->as_mut<Lam>()) : fe::Vector<Lam*>();
        auto n     = stack_.size();
        open_.emplace(mut);

        if (chain.empty()) {
            if (auto var = mut->has_var()) stack_.emplace_back(mut, Vars(var));
            for (auto op : mut->deps())
                schedule_(op);
        } else {
            for (auto* lam : chain) {
                done_.emplace(lam);
                if (auto var = lam->has_var()) stack_.emplace_back(mut, Vars(var));
            }
            for (auto* lam : chain) {
                schedule_(lam->type());
                schedule_(lam->filter());
                if (lam == chain.back()) schedule_tail(lam->body());
            }
        }

        stack_.resize(n);
        open_.erase(mut);
        place(mut, lvl); // after its ops: a decl this one needs comes first
    }

    /// A Lam's body is the tail of its block, so it is emitted there instead of as a `let` of its own.
    void schedule_tail(const Def* def) {
        if (!def || isa_decl(def)) return schedule_(def);
        if (!done_.emplace(def).second) return;
        for (auto op : def->deps())
            schedule_(op);
    }

    /// The innermost Frame whose Var%s @p def mentions; Top, if it mentions none.
    size_t level(const Def* def) const {
        if (stack_.empty()) return Top;
        if (mode_ == Dump::Local) return stack_.size() - 1; // no free-var analysis in the mode meant for debugging
        for (size_t i = stack_.size(); i-- != 0;)
            if (def->has_free_vars_in(stack_[i].vars)) return i;
        return Top;
    }

    void place(const Def* def, size_t lvl) {
        if (!placed_.emplace(def).second) return; // a root that is also reachable from another one
        (lvl == Top ? top_ : bucket_[stack_[lvl].key]).emplace_back(def);
    }

    bool enter(Def* mut, size_t lvl) const {
        switch (mode_) {
            case Dump::Expr:
            case Dump::Local: return roots_.contains(mut);
            case Dump::Scope: return lvl != Top || roots_.contains(mut);
            case Dump::All: return true;
        }
        fe::unreachable();
    }
    ///@}

    /// @name emit
    ///@{
    void emit(Def* key) {
        auto defs = key ? std::move(bucket_[key]) : std::move(top_);
        for (auto def : defs)
            if (auto mut = isa_decl(def))
                emit_decl(mut);
            else
                emit_let(def);
    }

    void emit_let(const Def* def) {
        std::println(os_, "{}let {}: {} = {};", tab_, name(def), Op(def->type()), Full(def));
    }

    void emit_decl(Def* mut) {
        if (auto lam = mut->isa_mut<Lam>()) return emit_lam(lam);
        if (!mut->is_set()) return emit_unset(mut);
        // `rec` binds the name for the body - which only a self-referential mutable needs; `extern` is out either way.
        std::println(os_, "{}{} {} = {};", tab_, recursive_.contains(mut) ? "rec" : "let", id(mut), Full(mut));
    }

    /// Nothing declares a mutable that was never set, so leave a trace instead of an unreadable dump.
    void emit_unset(Def* mut) { std::println(os_, "{}// `{}: {}` is unset", tab_, id(mut), Op(mut->type())); }

    void emit_lam(Lam* lam) {
        auto chain = curry_chain(lam);
        auto last  = chain.back();
        auto fun   = isa_fun(last) && !shadows_ret(last);
        auto con   = Lam::isa_cn(last) && !fun;

        // A `fun` binds its `ret` continuation as `return`, so that is the name its Var has to print with.
        // A `fun` binds its `ret` continuation as `return`, so that is the name its Var has to print with.
        if (fun) {
            if (auto var = last->has_var()) {
                if (auto num = num_binders(last->type()->dom()); num == 1)
                    renames_.defs.emplace(var, "return");
                else
                    renames_.ret_projs.emplace(var, num - 1);
            }
        }
        // Only an `extern` may go without a body - that is what forward-declares a native function.
        // A Lam without a body forward-declares what a native translation unit provides - there is nothing else
        // it could be, and only an `extern` may go without a body.
        if (!last->is_set()) return emit_bodyless(lam, chain, fun, con);

        std::print(os_, "{}{}{} {}", tab_, external(lam), fun ? "fun" : con ? "con" : "lam", id(lam));
        for (auto* c : chain) {
            os_ << ' ';
            auto num   = num_binders(c->type()->dom());
            auto limit = fun && c == last ? num - 1 : num;
            curry(os_, c->has_var(), c->type()->dom(), c->type()->is_implicit(), num, limit, !fun || c != last);
            auto& w = c->world();
            // A CPS Lam's last domain defaults to `ff`, every other one to `tt`; only spell out the rest.
            if (auto dflt = c == last && (fun || con) ? w.lit_ff() : w.lit_tt(); c->filter() != dflt)
                std::print(os_, "@({})", Op(c->filter()));
        }

        if (fun)
            std::print(os_, ": {} =", Op(last->ret_dom()));
        else if (!con)
            std::print(os_, ": {} =", Op(last->type()->codom()));
        else
            os_ << " =";
        os_ << '\n';

        ++tab_;
        rets_.emplace_back(fun ? last->has_var() : nullptr);
        emit(lam);
        emit_tail(last->body(), ";");
        rets_.pop_back();
        --tab_;
    }

    /// A Lam without a body only ever declares what some backend provides: `extern con f [mem.M 0, I32];`.
    void emit_bodyless(Lam* lam, const fe::Vector<Lam*>& chain, bool fun, bool con) {
        auto last = chain.back();
        auto dom  = last->type()->dom();
        std::print(os_, "{}extern {} {}", tab_, fun ? "fun" : con ? "con" : "lam", id(lam));
        auto num = num_binders(dom);
        if (fun) {
            os_ << '[';
            for (auto sep = ""; auto i : std::views::iota(size_t(0), num - 1)) {
                std::print(os_, "{}{}", sep, Op(dom->proj(num, i)));
                sep = ", ";
            }
            std::println(os_, "]: {};", Op(last->ret_dom()));
        } else {
            std::println(os_, "[{}]{};", Op::map(dom->projs(num)),
                         con ? std::string() : std::format(": {}", Op(last->type()->codom())));
        }
    }

    /// Tail position: what a block ends with is spelled out - it has no `let` of its own.
    void emit_tail(const Def* def, std::string_view end) {
        if (isa_decl(def))
            std::println(os_, "{}{}{}", tab_, Op(def), end);
        else
            std::println(os_, "{}{}{}", tab_, Full(def), end);
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
    fe::Tab tab_ = fe::Tab::spaces();
    fe::Vector<Frame> stack_;
    fe::Vector<const Var*> rets_;
    DefVec top_;
    MutMap<DefVec> bucket_;
    DefSet done_;
    DefSet placed_;
    DefSet roots_;
    MutSet open_; ///< On the schedule stack - a Def reaching one of these is recursive.
    MutSet recursive_;
    Renames renames_;
    fe::BFSWorklist<MutSet> muts_;
};

} // namespace

/*
 * Def
 */

/// This will stream @p def as an operand.
/// This is usually `id(def)` unless it can be displayed Inline.
std::ostream& operator<<(std::ostream& os, const Def* def) {
    if (def == nullptr) return os << "<nullptr>";
    if (auto d = Full(def)) {
        auto _ = def->world().freeze();
        return os << d;
    }
    return os << id(def);
}

std::ostream& Def::stream(std::ostream& os, Dump mode) const {
    auto _ = world().freeze();
    if (mode == Dump::Expr) return os << this << std::endl;

    auto dumper = Dumper(os, mode);
    dumper.schedule(this);
    dumper.emit_top();
    if (!isa_decl(this)) dumper.emit_expr(this);
    return os;
}

void Def::dump() const { std::cout << this << std::endl; }
void Def::dump(Dump mode) const { stream(std::cout, mode); }

void Def::write(Dump mode, const char* file) const {
    auto ofs = std::ofstream(file);
    stream(ofs, mode);
}

void Def::write(Dump mode) const {
    auto file = id(this) + ".mim"s;
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
    // An `import` re-declares whatever it declared the first time around, so dumping it again is a redefinition.
    auto imported = [&srcs](const Def* def) { return srcs.contains(def->loc().src); };

    for (const auto& import : driver().imports()) {
        auto kw = import.tag == ast::Tok::Tag::K_plugin ? "plugin" : "import";
        // The spelling was relative to the importing file; only the resolved path re-parses from here.
        // Generic format: a native Windows `\` would lex as an escape sequence inside the string literal.
        if (import.path)
            std::print(os, "{} \"{}\";\n", kw, ast::Lexer::escape(import.src->path().generic_string()));
        else
            std::print(os, "{} {};\n", kw, import.sym);
    }

    // The recursive dump keeps every mutable to itself: no free-var analysis that a broken program could trip.
    auto dumper = Dumper(os, flags().dump_recursive ? Def::Dump::Local : Def::Dump::All);
    for (auto mut : externals().muts())
        if (!imported(mut)) dumper.schedule(mut);
    dumper.emit_top();
    dumper.drain();

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
