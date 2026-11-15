#pragma once

#include <pbqp/graph.h>

#include <mim/phase.h>

namespace mim::plug::x86::phase {

class ISel;

/// @name Rules
/// A rule rewrites a pattern over `core`/`mem` into an x86 term; the holes of the pattern are typed.
///@{

/// The type of a rule's replacement.
enum class Ty : u8 {
    Reg,   ///< What the pattern's root yields, in registers.
    Base,  ///< `x86.Base`.
    Index, ///< `x86.Index`.
};

/// The type of a hole.
enum class Want : u8 {
    Reg,   ///< A register.
    Imm,   ///< A literal that fits an immediate; it is a pattern node rather than a hole.
    Base,  ///< `x86.Base`.
    Index, ///< `x86.Index`.
    Addr,  ///< `Addr i` for any `i`.
};

/// What a match needs of the operand @p def: a hole of type @p want or, if @p inner is set, that inner pattern node.
struct Demand {
    const Def* def;
    Want want = Want::Reg;
    u64 inner = 0;
};

/// An inner node a match covers, with what it needs of its own operands.
struct Cover {
    const Def* def;
    u64 inner;
    fe::Vector<Demand> demands;
};

struct Rule;

/// An alternative of a PBQP node: a match rooted there, or the class of all matches covering it as the same inner node.
struct Alt {
    const Rule* rule = nullptr; ///< `nullptr` for an inner alternative.
    u64 inner        = 0;
    pbqp::Cost cost  = 0;
    fe::Vector<Demand> demands;
    fe::Vector<Cover> covers;
    std::array<const Def*, 3> bind{}; ///< What the matcher bound for Rule::build.
};

using Alts = fe::Vector<Alt>;

struct Rule {
    std::string_view name;
    Ty type;
    /// Builds the replacement of the match @p alt rooted at the old_world() Def @p root.
    const Def* (*build)(ISel&, const Def* root, const Alt& alt);
};
///@}

/// Rewrites the `core` and `mem` operations of a lowered world into x86 instructions.
/// Pointers and `Nat` become `I64`; everything else this phase does not know is an error.
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

    /// `base + index * scale + disp` in new_world(); a missing `index` is `nullptr`.
    struct Address {
        const Def* base;
        const Def* index = nullptr;
        nat_t scale      = 1;
        nat_t disp       = 0;
    };

    /// @name For Rules
    ///@{
    size_t uses(const Def* def) const { return uses_.contains(def) ? uses_.at(def) : 0; }
    /// The `mem.load` of old_world() that @p def is the value of, if nothing else uses this value.
    const App* sole_load(const Def* def) const;
    /// The old_world() operand @p def as @p want in new_world().
    const Def* hole(const Def* def, Want want = Want::Reg);
    /// The rewritten @p def with a `Nat` literal as `I64`, for value operands.
    const Def* val(const Def* def);
    /// Zero-extends @p x of new_world() to `Idx d`, folding literals.
    const Def* zext(const Def* d, const Def* x);
    /// The `x86.addr`/`x86.index` term of @p a.
    const Def* addr(Address a);
    /// The Address of an `x86.addr`/`x86.index` term.
    Address split(const Def* addr);
    /// @p a as a value.
    const Def* materialize(Address a);
    ///@}

private:
    bool analyze() final;
    /// Counts in `uses_` how often each Def of old_world() occurs as an operand.
    void count(const Def*);
    /// The node Def @p def stands for: a load stands for its value.
    const Def* key(const Def* def) const;
    /// The chosen alternative of the node @p def.
    const Alt* choice(const Def* def) const;
    /// The replacement of the node @p def as its rule built it.
    const Def* select(const Def* def);
    /// The replacement of @p def in registers, if @p def is a node.
    const Def* selected(const Def* def);
    /// @p x of type @p from as @p want.
    const Def* convert(const Def* x, Ty from, Want want);

    const Def* rewrite_imm_Nat(const Nat*) final { return new_world().type_i64(); }
    const Def* rewrite_imm_Lit(const Lit*) final;
    const Def* rewrite_imm_App(const App*) final;
    const Def* rewrite_imm_Extract(const Extract*) final;
    const Def* coerce_leaf(const Def*, const Def*) final;

    DefMap<size_t> uses_;
    DefSet counted_;
    DefMap<Alts> alts_;
    DefMap<size_t> choices_;
    Def2Def roots_; ///< inner node → root of the match covering it
    Def2Def repls_;
};

} // namespace mim::plug::x86::phase
