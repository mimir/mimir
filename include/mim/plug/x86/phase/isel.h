#pragma once

#include <pbqp/graph.h>

#include <mim/phase.h>

#include "mim/plug/x86/rule_table.h"

namespace mim::plug::x86::phase {

/// @name Rules
/// A rule rewrites a pattern over `core`/`mem` into an x86 term; the holes of the pattern are typed.
///@{

using rules::Ty;
using rules::Want;

/// What a match needs of the operand @p def: a hole of type @p want or, if @p inner is set, that inner pattern node.
/// While matching, @p inner is one plus the index of its Cover; `analyze()` turns it into the id of the inner
/// alternative.
struct Demand {
    const Def* def;
    Want want    = Want::Reg;
    size_t inner = 0;

    bool operator==(const Demand&) const = default;
};

/// An inner node a match covers, with what it needs of its own operands.
struct Cover {
    const Def* def;
    size_t inner = 0;
    fe::Vector<Demand> demands;
    bool through = false; ///< The match only looks through it, since something else uses it.
};

/// An alternative of a PBQP node: a match rooted there, or the class of all matches covering it as the same inner node.
struct Alt {
    const rules::Rewrite* rule = nullptr; ///< `nullptr` for an inner alternative, whose id is its index plus one.
    fe::Vector<Demand> demands;
    fe::Vector<Cover> covers;
    DefVec metas;   ///< What the meta variables of its rule match.
    u32 copies = 0; ///< The operands its two-address instructions overwrite although something else uses them.
};

using Alts = fe::Vector<Alt>;

///@}

/// Rewrites the `core` and `mem` operations of a world that `x86.legalize` lowered into x86 instructions.
/// Everything this phase does not know is an error.
///
/// Selection is a cover of the program by matches of rules, found as a PBQP (Buchwald & Zwinkau, CASES 2010), where
/// the holes of a pattern are typed.
/// Every operation is a node; its alternatives are the matches rooted there and the inner nodes other matches cover it
/// with.
/// An edge prices the conversion between the type of the operand's rule and the type of the user's hole.
/// The first alternative of every node is an atomic rule, so choosing it everywhere is a valid cover.
class ISel : public RWPhase {
public:
    using RWPhase::RWPhase;

private:
    /// @name For Rules
    ///@{
    size_t uses(const Def* def) const {
        auto n = fe::lookup(uses_, def);
        return n ? *n : 0;
    }
    /// Does overwriting @p def cost a copy, since something else uses it?
    /// The other uses of a parameter are mostly in other continuations, so it never does.
    bool overwritten(const Def* def) const;
    /// The memory operation of old_world() that @p def is the value of, if nothing else uses this value.
    const App* sole_value(const Def* def) const;
    /// The old_world() operand @p def as @p want in new_world().
    const Def* hole(const Def* def, Want want = Want::Reg);
    /// Builds the rhs of the rule of @p alt.
    const Def* build(const Alt& alt);
    ///@}

    bool analyze() final;
    /// Counts in `uses_` how often each Def of old_world() occurs as an operand.
    void count(const Def*);
    /// Appends the matches rooted at @p def to @p alts.
    void match(const Def* def, Alts& alts);
    /// Appends the matches of @p rule rooted at @p root to @p alts.
    void append(const rules::Rewrite& rule, const Def* root, Alts& alts);
    /// Appends the match of @p rule rooted at @p root with the commuting nodes in @p swap swapped and yields its
    /// copies. A swap of `imm_swaps` without an immediate only counts if it needs fewer copies than the @p rival
    /// without it.
    std::optional<u32>
    append(const rules::Rewrite& rule, const Def* root, u32 swap, std::optional<u32> rival, Alts& alts);
    /// The inner node the operand @p def of @p parent stands for, if nothing outside the match uses it.
    const App* sole(const Def* parent, const Def* def) const;
    /// Does the match @p alt meet the side condition of its rule?
    bool holds(const Alt& alt);
    /// The node Def @p def stands for: a memory operation stands for its value.
    const Def* key(const Def* def) const;
    /// The chosen alternative of the node @p def.
    const Alt* choice(const Def* def) const;
    /// The replacement of the node @p def as its rule built it.
    const Def* select(const Def* def);
    /// The replacement of @p def in registers, if @p def is a node.
    const Def* selected(const Def* def);
    /// @p x of non-terminal @p from as @p want, by the cheapest chain of conversion rules.
    const Def* convert(const Def* x, Ty from, Want want);

    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;

    DefMap<size_t> uses_;
    DefSet counted_;
    DefMap<Alts> alts_;
    DefMap<size_t> choices_;
    Def2Def roots_; ///< inner node → root of the match covering it
    Def2Def repls_;
};

} // namespace mim::plug::x86::phase
