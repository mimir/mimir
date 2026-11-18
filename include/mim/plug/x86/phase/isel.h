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
};

using Alts = fe::Vector<Alt>;

struct Rule {
    std::string_view name;
    Ty type;
    /// Builds the replacement of the match @p alt rooted at the old_world() Def @p root.
    const Def* (ISel::*build)(const Def* root, const Alt& alt);
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
        u8 scale         = 1;
        s64 disp         = 0;
    };

    /// @name For Rules
    ///@{
    size_t uses(const Def* def) const {
        auto n = fe::lookup(uses_, def);
        return n ? *n : 0;
    }
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
    /// Applies @p insn, going through `i32` for operands narrower than 16 bits.
    const Def*
    wide(const Def* (*insn)(World&, const Def*, const Def*), const Def* type, bool sign, const Def* a, const Def* b);

    const Def* build_lea_off(const Def* root, const Alt& alt);
    const Def* build_lea_idx(const Def* root, const Alt& alt);
    const Def* build_load(const Def* root, const Alt& alt);
    const Def* build_store(const Def* root, const Alt& alt);
    const Def* build_store_mr(const Def* root, const Alt& alt);
    const Def* build_bin(const Def* root, const Alt& alt);
    const Def* build_bin_rm(const Def* root, const Alt& alt);
    const Def* build_cmp(const Def* root, const Alt& alt);
    const Def* build_nat(const Def* root, const Alt& alt);
    const Def* build_unary(const Def* root, const Alt& alt);
    const Def* build_div(const Def* root, const Alt& alt);
    const Def* build_slot(const Def* root, const Alt& alt);
    const Def* build_assume(const Def* root, const Alt& alt);
    const Def* build_select(const Def* root, const Alt& alt);
    ///@}

private:
    bool analyze() final;
    /// Counts in `uses_` how often each Def of old_world() occurs as an operand.
    void count(const Def*);
    /// Appends the matches rooted at @p def to @p alts, an atomic one first.
    void match(const Def* def, Alts& alts) const;
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
