#pragma once

#include "mim/nest.h"

#include "printer.h"

namespace mim::dump {

using Srcs = ankerl::unordered_dense::set<const fe::Src*>;

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
    void generics(Generics generics) { plan().generics = std::move(generics); }
    /// Switches to plain names; @p reserved are names the dump refers to verbatim and hence must not pick.
    void plain(std::span<const std::string> reserved) { p_.namer().plain(reserved); }

    /// Declares the annexes of the dumped file itself - Axm%s as `axm`s, Nom%s as `nom`s - grouped into the
    /// `mod` their names nest them in.
    void dump_annexes(std::string self, std::span<const DumpAnnex> annexes);

    /// @name dump
    ///@{
    /// @p def and whatever Dumper::mode_ reaches from it.
    void dump(const Def* def);
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
        bucket();
        emit(nullptr);
    }

    /// @name schedule
    ///@{
    /// The closed mutables @p muts reach, callees first: Mim binds a name before its uses.
    /// A non-decl @p tail ends the dump; it is spelled out after the `let`s it needs.
    void dump_muts(fe::View<Def*> muts, const Def* tail = nullptr);
    static bool is_spellable(const Def* arm);
    /// Like mim::post_order, but visits the mutables in the order the operands reach them.
    /// Def::local_muts orders by pointer, which a re-read dump need not reproduce.
    void post_order(Def* mut, fe::Vector<Def*>& todo);
    void local_muts(const Def* def, DefSet& seen, fe::Vector<Def*>& muts);
    /// Records that @p op is an arm of @p match or, if `nullptr`, used otherwise.
    void note_use(const Def* op, const Match* match);

    /// The Def%s the dump prints @p def with: a type nobody prints must not drag in a `let` that nothing refers to.
    /// A Lam spells out its Pi, so only the components of that one are referenced.
    void printed_deps(const Def* def, auto f) const {
        if (p_.isa_inst(def)) {
            for (auto arg : def->as<Nom>()->args())
                f(arg);
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

    void note_vars(const Def* def);
    /// @p let tells whether a `let` binds @p def, should it not print Full.
    void schedule_(const Def* def, bool let = true);
    void schedule_deps(const Def* def);
    /// A mutable reached through @p def%'s deps may need @p def - and hence all of its deps - before they are done.
    void finish(const Def* def);
    void schedule_mut(Def* mut);
    /// A block only sees what precedes it, so the mutually recursive siblings of @p mut print as one `mutual` group.
    fe::Vector<Def*> sibling_group(Def* mut) const;
    bool groupable(Def* mut) const;
    void schedule_body(Def* mut);
    /// A destructured parameter prints the types of its projections, where the Lam's own Var is already substituted.
    void schedule_proj_types(const Def* def);
    /// Schedules what @p def needs; @p def itself, noted in @p inline_set, prints inline.
    void schedule_inline(const Def* def, DefSet& inline_set);
    /// A Lam's body is the tail of its block, so it is emitted there instead of as a `let` of its own.
    void schedule_tail(const Def* def);
    /// Is @p mut ours to emit - or does it only get referenced by name?
    /// Dump::Scope only enters what nests in one of Dumper::scope_.
    bool enter(Def* mut) const;
    /// Turns the schedule into one list per block.
    void bucket();
    /// Does @p mut print inline - be it one itself, or because it sits inside one that does?
    bool inlines(Def* mut) const;
    bool is_recursive(Def* mut) const { return (*nest_)[mut]->is_recursive(); }
    /// The mutable whose block @p node stands for; `nullptr` for the top level.
    Def* owner(const Nest::Node* node) const;
    ///@}

    /// @name emit
    ///@{
    void emit(Def* key);
    void emit_item(const Def* def);
    void emit_nom(std::string_view kw, std::string_view name, const Nom* nom);
    /// The right-hand side of a declaration; only a bare variant may refer back to the name it declares.
    void emit_rhs(Op rhs);
    /// Groups the Nom%s among @p defs that lie on a cycle, in the order of @p defs.
    /// No immutable refers back to itself, so such a cycle runs through a recursive SCC.
    /// Everything a cycle needs precedes its last member, so that is where the group goes.
    void group_noms(const Nest& nest, const DefVec& defs);
    /// The Nom%s @p scc reaches that reach it back.
    static DefSet scc_noms(const Nest& nest, const Nest::Node::SCC& scc);

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
    void sep(Prev curr);
    void emit_let(const Def* def);
    void emit_decl(Def* mut);
    void emit_rule(Rule* rule);
    /// Nothing declares a mutable that was never set, so leave a trace instead of an unreadable dump.
    void emit_unset(Def* mut);
    void emit_lam(Lam* lam);
    /// A Lam without a body only ever declares what some backend provides: `extern con f [mem.M 0, I32];`.
    void emit_bodyless(Lam* lam, Lam* last, LamKind);
    void emit_tail(const Def* def, std::string_view end);
    /// Would a `fun` here hide the `return` of an enclosing one @p lam might still refer to?
    /// The `return` is a projection of its Lam's Var, so capturing that Var at all is as precise as this gets.
    bool shadows_ret(Lam* lam) const;
    ///@}

    std::ostream& os_;
    Def::Dump mode_;
    bool typed_let_;
    const Nest* nest_ = nullptr; ///< Nests the mutables of the scope being emitted.
    Prev prev_        = Prev::None;
    fe::Vector<const Var*> rets_;
    DefVec order_; ///< What to emit, in dependency order.
    DefVec top_;
    MutMap<DefVec> bucket_;
    MutMap<Def*> absorbed_; ///< Inner Lam of a curried chain -> the chain's outermost one.
    DefSet done_;
    DefSet scope_;          ///< The closed mutables Dump::Scope dumps.
    DefSet pending_;        ///< Def%s whose deps are being scheduled.
    DefMap<DefVec> mutual_; ///< The member a `mutual` group prints at -> the whole group.
    DefSet deferred_;       ///< The other members of a `mutual` group, which print with it.
    Printer p_;
    Srcs srcs_;
    MutSet scheduled_;
    /// A `match` a Lam is an arm of, or `nullptr` if it is used otherwise and hence needs a declaration.
    /// Re-reading spells out an arm per `match` anyway, so one shared by several is spelled out in each.
    DefMap<const Match*> arm_of_;
};

} // namespace mim::dump
