#pragma once

#include <array>
#include <optional>
#include <string_view>

#include <pbqp/graph.h>

#include <mim/phase.h>

#include "mim/plug/x86/rule_table.h"

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
    Lit,   ///< No rule's type, but that of a literal operand.
    Num,
};

/// The type of a hole.
enum class Want : u8 {
    Reg,   ///< A register.
    Imm,   ///< A literal that fits a sign-extended `i32` immediate.
    Sh,    ///< A literal shift count, which x86 masks; any literal fits.
    Base,  ///< `x86.Base`.
    Index, ///< `x86.Index`.
    Addr,  ///< `Addr i` for any `i`.
    Num,
};

/// Does a hole of type @p want take a literal rather than a replacement?
constexpr bool is_literal(Want want) { return want == Want::Imm || want == Want::Sh; }

/// The operation a rule selects, one per instruction of the pattern surface.
#define MIM_X86_ISEL_OPS(X) \
    X(Select)               \
    X(Ptradd)               \
    X(Load)                 \
    X(Store)                \
    X(WrapAdd)              \
    X(WrapSub)              \
    X(WrapMul)              \
    X(WrapShl)              \
    X(BitAnd)               \
    X(BitOr)                \
    X(BitXor)               \
    X(BitNand)              \
    X(BitNor)               \
    X(BitNxor)              \
    X(ShrA)                 \
    X(ShrL)                 \
    X(Icmp)                 \
    X(Ncmp)                 \
    X(NatAdd)               \
    X(NatSub)               \
    X(NatMul)               \
    X(NatDiv)               \
    X(NatRem)               \
    X(Conv)                 \
    X(Idx)                  \
    X(Idx2Nat)              \
    X(Bitcast)              \
    X(Rem)                  \
    X(Div)                  \
    X(Slot)                 \
    X(Num)

enum class Op : u8 {
#define X(op) op,
    MIM_X86_ISEL_OPS(X)
#undef X
};

/// A side condition of a rule beyond what its slots state.
enum class Cond : u8 {
    None,
    Fold, ///< The replacement sits at the inner node whose memory it replaces, so the root's holes must be available
          ///< there.
};

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

struct Rule;
struct Pattern;

/// An alternative of a PBQP node: a match rooted there, or the class of all matches covering it as the same inner node.
struct Alt {
    const Rule* rule = nullptr; ///< `nullptr` for an inner alternative, whose id is its index plus one.
    pbqp::Cost cost  = 0;
    fe::Vector<Demand> demands;
    fe::Vector<Cover> covers;
    DefVec metas; ///< What the meta variables of a generated rule match.
};

using Alts = fe::Vector<Alt>;

/// Builds the replacement of the match @p alt rooted at the old_world() Def @p root.
using BuildFn = const Def* (ISel::*)(const Def*, const Alt&);

/// One pattern ↦ replacement: the root's operation, the holes of its operands, the replacement's type, cost, and
/// builder. A rule of `x86_isel` may also have inner nodes; its holes are those of its operand meta variables.
struct Rule {
    static constexpr size_t Max_Slots = 6;

    Op op;
    Ty type;
    pbqp::Cost cost;
    std::string_view name;
    BuildFn build;
    Cond cond    = Cond::None;
    u8 num_slots = 0;
    std::array<Want, Max_Slots> slots{};
    const rules::Rewrite* gen = nullptr; ///< The rule of `x86_isel` this one stems from.
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
    /// The memory operation of old_world() that @p def is the value of, if nothing else uses this value.
    const App* sole_value(const Def* def) const;
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
    /// Does the x86 instruction @p insn take operands of `Idx size`?
    bool supports(flags_t insn, nat_t size);

    const Def* build_slot(const Def* root, const Alt& alt);
    const Def* build_select(const Def* root, const Alt& alt);
    /// Builds the rhs of a generated rule.
    const Def* build_gen(const Def* root, const Alt& alt);
    ///@}

private:
    bool analyze() final;
    /// Counts in `uses_` how often each Def of old_world() occurs as an operand.
    void count(const Def*);
    /// An operation of old_world() as a pattern node: its `Op`, its operands, and whether they commute.
    struct Node {
        const Pattern* ptrn;
        Op op;
        DefVec ops;
    };

    /// @p def as a pattern node, if a pattern covers it.
    std::optional<Node> decompose(const Def* def) const;
    /// Appends the matches rooted at @p def to @p alts.
    void match(const Def* def, Alts& alts);
    /// Appends the matches of the generated @p rule rooted at @p root to @p alts.
    void append_gen(const Rule& rule, const Def* root, Alts& alts);
    /// Appends the match of the generated @p rule rooted at @p root with the commuting nodes in @p swap swapped.
    void append_gen(const Rule& rule, const Def* root, u32 swap, Alts& alts);
    /// Appends the match of the hand-written @p rule on @p node to @p alts.
    void append(const Rule& rule, const Node& node, Alts& alts);
    /// The inner node the operand @p def of @p parent stands for, if nothing outside the match uses it.
    const App* sole(const Def* parent, const Def* def) const;
    /// Does the match @p alt meet the side condition of its rule?
    bool holds(const Alt& alt);
#ifdef MIM_ENABLE_CHECKS
    /// Verifies atomic completeness, total conversions, and inner-node coverage over the rule set.
    void check_rules() const;
#endif
    /// The node Def @p def stands for: a memory operation stands for its value.
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

    ankerl::unordered_dense::map<std::pair<flags_t, nat_t>, bool> supports_;
    DefMap<size_t> uses_;
    DefSet counted_;
    DefMap<Alts> alts_;
    DefMap<size_t> choices_;
    Def2Def roots_; ///< inner node → root of the match covering it
    Def2Def repls_;
};

} // namespace mim::plug::x86::phase
