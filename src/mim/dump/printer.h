#pragma once

#include <ostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include <fe/format.h>

#include "mim/world.h"

#include "mim/ast/tok.h"

// A dump prints a Def that appears as an operand in one of two ways:
// * Full: spelled out with all of its operands, e.g. `(1, 2, 3)`; see Printer::is_full.
// * By name (see Printer::id): everything else; a `let` - or a declaration of its own for a decl - binds it.
// A mutable that is no decl is printed like an immutable.

namespace mim::dump {

using ast::Prec;

/// An `anx nom` carries its qualified `<file>.<name>` sym, which no identifier can spell; see Annexes::attach.
bool is_anx_nom(const Def*);
Def* isa_decl(const Def*);

/// The Lam%s a curried declaration folds into one: `lam f (a) (b) = e`.
fe::Vector<Lam*> curry_chain(Lam*);

/// Def::num_tprojs, except that a *fused* Seq stays one binder: dumping freezes the World and its sub-Seqs
/// need not exist as Def%s.
nat_t num_binders(const Def* type);

/// Each curried argument of an application and whether its Pi is implicit.
using Args = fe::Vector<std::pair<DefVec, bool>>;

/// An instance of a generic `anx nom` from elsewhere: its annex's qualified name and arguments.
/// Each curried argument holds either the whole argument or the components a re-read dump tuples up again.
using Insts = DefMap<std::pair<Sym, Args>>;

/// The surface syntax a Lam prints with; a `fun` is sugar for a Lam of the shape `[<params>, Cn <ret>]`.
enum class LamKind { Lam, Con, Fun };

/// The kind of @p lam, where @p fun tells whether it may print as a `fun`.
LamKind lam_kind(const Lam* lam, bool fun);
/// The keyword that declares a Lam of that kind.
std::string_view decl_kw(LamKind);

/// What encloses a binder: `()` for a pattern, `[]` for a type, and `{}` for an implicit one of either.
struct Delim {
    static Delim ptrn(bool implicit = false) { return implicit ? Delim{'{', '}', false} : Delim{'(', ')', false}; }
    static Delim type(bool implicit = false) { return implicit ? Delim{'{', '}', true} : Delim{'[', ']', true}; }

    char l, r;
    bool brckt; ///< Whether the sub-patterns are those of a type.
};

/// Picks the identifier a Def prints by.
class Namer {
public:
    /// Switches to plain names; @p reserved are names the dump refers to verbatim and hence must not pick.
    void plain(std::span<const std::string> reserved);

    /// `<world name>.`: the dumped file's own annexes are spelled without it.
    void self(std::string self) { self_ = std::move(self); }
    /// `<mod>.` while declaring the Axm%s of that `mod`, which does not see its own name.
    void mod(std::string mod) { mod_ = std::move(mod); }

    const std::string* lookup(const Def* def) const { return fe::lookup(names_, def); }
    /// Names @p def @p name unless it has a name already; yields the name it goes by.
    const std::string& bind(const Def* def, std::string name) {
        return names_.emplace(def, std::move(name)).first->second;
    }
    /// The @p i-th projection of @p var is the `ret` continuation a `fun` calls `return`, whatever its Def::sym.
    /// Keyed by Var and index, because the projection itself need not exist as a Def while the World is frozen.
    void ret(const Def* var, nat_t i) { ret_projs_.emplace(var, i); }

    /// Def::unique_name - or a plain name while dumping a World or formatting a diagnostic, where a gid is noise.
    std::string name(const Def*);
    /// A numbered variant of @p base that no other name of the dump takes.
    std::string fresh(const std::string& base);
    /// Forgets the names of @p var and its projections, so that the next spelling picks fresh ones.
    void unname(const Var*);
    /// The name an annex prints by: the dumped file's own annexes drop the `<file>.` - and the `<mod>.` - prefix.
    std::string annex(Sym) const;

private:
    std::string pick(const Def*);

    DefMap<std::string> names_;
    DefMap<nat_t> ret_projs_;
    /// Distinct identifiers picked instead of Def::unique_name, so a re-read dump keeps them as its Def::sym%s.
    ankerl::unordered_dense::set<std::string> taken_;
    ankerl::unordered_dense::map<std::string, size_t> counters_; ///< Next numbered variant to try per base name.
    bool plain_ = false;
    std::string self_;
    std::string mod_;
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

/// A Def as an operand of the expression at hand: Full if Printer::is_full - or forced to -, and by name otherwise.
class Op {
public:
    Op(Printer& printer, const Def* def, Prec prec = Prec::Bot, bool is_left = false, bool full = false)
        : printer_(printer)
        , def_(def)
        , prec_(prec)
        , is_left_(is_left)
        , full_(full) {}

    /// @name Sub-expressions
    ///@{
    Op op(const Def* def, Prec prec = Prec::Bot, bool is_left = false) const { return {printer_, def, prec, is_left}; }
    Op l(const Def* def, Prec prec = Prec::Bot) const { return {printer_, def, prec, true}; }
    Op r(const Def* def, Prec prec = Prec::Bot) const { return {printer_, def, prec, false}; }
    ///@}

    /// @name Getters
    ///@{
    Printer& printer() const { return printer_; }
    Prec prec() const { return prec_; }
    bool is_left() const { return is_left_; }
    bool full() const { return full_; }
    const Def* def() const { return def_; }
    const Def* operator->() const { return def_; }
    const Def* operator*() const { return def_; }
    ///@}

private:
    Printer& printer_;
    const Def* def_;
    Prec prec_;
    bool is_left_;
    bool full_;

    friend std::ostream& operator<<(std::ostream&, Op);
};

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
    Namer& namer() { return namer_; }
    fe::Tab& tab() { return tab_; }
    ///@}

    /// @name Operands
    ///@{
    Op op(const Def* def, Prec prec = Prec::Bot) { return {*this, def, prec}; }
    Op full(const Def* def) { return {*this, def, Prec::Bot, false, true}; }
    /// A block's tail has no `let`, so it is spelled out unless it has a declaration of its own.
    Op tail(const Def* def) { return has_decl(def) ? op(def) : full(def); }
    auto ops(const auto& range, const char* sep = ", ", Prec prec = Prec::Bot) {
        return fe::Join(range | std::views::transform([this, prec](auto def) { return Op(*this, def, prec); }), sep);
    }
    ///@}

    /// @name Names
    ///@{
    std::string name(const Def*);
    std::string id(const Def*);
    ///@}

    /// @name Queries
    ///@{
    const std::pair<Sym, Args>* isa_inst(const Def*) const;
    /// Does the Def print by name, bound by a declaration of its own - be it a decl or a `nom`?
    bool has_decl(const Def*) const;
    /// Does the Def always print Full - instead of by name?
    bool is_full(const Def*) const;
    /// Is the Lam sugar for a `fun`?
    /// A `fun` binds only its parameters, so a Var used as a whole needs the `con` spelling.
    bool isa_fun(const Lam*) const;
    ///@}

    /// @name Patterns
    ///@{
    /// Whether @p def of @p type comes apart into @p n components; by_name agrees with that from then on.
    bool split(const Def* def, const Def* type, size_t n);
    /// A binder within @p delim: `(a: A, b: B) as x` if it comes apart into @p parts, `(x: T)` if not.
    /// @p num is how many components it has; @p limit how many of them to print - a returning Lam hides its last.
    void bndr(std::ostream&, Delim, const Def* def, const Def* type, size_t num, size_t limit, bool parts, bool alias);
    /// The parameter lists of a curried @p chain and its result type.
    void lam_header(std::ostream&, fe::View<Lam*> chain, LamKind);
    /// The names of the constructors live in the frontend, so each case prints under its index.
    std::string ctors(const Variant*);
    ///@}

private:
    /// @name Expressions
    ///@{
    Prec prec(const Def*) const;
    bool needs_parens(Op) const;
    void print(std::ostream&, Op);
    void print_full(std::ostream&, Op);
    void print_inst(std::ostream&, Op, const std::pair<Sym, Args>&);
    void print_lit(std::ostream&, Op, const Lit*);
    void print_insert(std::ostream&, Op, const Insert*);
    void print_pi(std::ostream&, Op, const Pi*);
    void print_lam(std::ostream&, Lam*);
    void print_app(std::ostream&, Op, const App*);
    void print_sigma(std::ostream&, Op, const Sigma*);
    void print_inj(std::ostream&, Op, const Inj*);
    void print_match(std::ostream&, Op, const Match*);
    std::string shape(const Seq*);
    std::string shape(const Seq*, const Def* var);
    ///@}

    /// @name Patterns
    ///@{
    const Def* live_proj(const Def*, nat_t n, nat_t i) const;
    bool is_opaque(const Def*) const;
    const Def* alias_proj(const Def*) const;
    bool by_name(const Extract*) const;
    bool destructible(const Def* def, const Def* type, size_t n) const;
    void alias_sigma(const Def* type, const Def* def);
    void whole(std::ostream&, const Def* def, const Def* type);
    void ptrn(std::ostream&, const Def* def, const Def* type, bool brckt = false, const Def* binder = nullptr);
    void ptrns(std::ostream&, const Def* def, const Def* type, size_t n, size_t limit, bool brckt);
    void lam_ptrn(std::ostream&, Lam*, LamKind, bool last, bool alias);
    void lam_codom(std::ostream&, Lam*, LamKind);
    ///@}

    bool expr_;
    bool in_header_ = false;
    fe::Tab tab_    = fe::Tab::spaces(); ///< Indentation of the line being printed.
    Plan plan_;
    Namer namer_;
    DefMap<bool> split_; ///< Whether a pattern took a Def apart, so that its components print by name.
    DefSet bound_;       ///< Projections of a dependent Sigma's Var a pattern binds in place of a dead component.
    DefSet spelled_;     ///< Arms already spelled out; a re-read dump binds fresh names for each further spelling.
    /// A Lam's codomain refers to its Pi's Var - or its dependent domain's - but only the Lam's Var is bound by name.
    DefMap<const Def*> aliases_;

    friend std::ostream& operator<<(std::ostream&, Op);
};

} // namespace mim::dump

#ifndef DOXYGEN
template<>
struct std::formatter<mim::dump::Op> : fe::ostream_formatter {};
#endif
