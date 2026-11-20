#pragma once

#include <array>
#include <optional>
#include <string_view>

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
    Imm,   ///< A literal that fits a sign-extended `i32` immediate; it is a pattern node rather than a hole.
    Sh,    ///< A literal shift count, which x86 masks; any literal fits.
    Base,  ///< `x86.Base`.
    Index, ///< `x86.Index`.
    Addr,  ///< `Addr i` for any `i`.
};

/// The operation a rule selects, one per instruction of the pattern surface.
#define MIM_X86_ISEL_OPS(X) \
    X(Select)               \
    X(LeaOffset)            \
    X(LeaScale)             \
    X(LeaMul)               \
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
    X(Assume)               \
    X(Alu)                  \
    X(Num)

/// `Op::Alu` is no instruction: as an inner node, it matches any operation with a read-modify-write memory form.
enum class Op : u8 {
#define X(op) op,
    MIM_X86_ISEL_OPS(X)
#undef X
};

/// One position of a rule's pattern in preorder: a hole, or an inner node whose @p arity operands follow it.
struct Slot {
    Want want = Want::Reg;
    Op node   = Op::Num; ///< `Op::Num` for a hole.
    u8 arity  = 0;

    constexpr Slot() = default;
    constexpr Slot(Want want)
        : want(want) {}
    constexpr Slot(Op node, u8 arity)
        : node(node)
        , arity(arity) {}

    constexpr bool is_hole() const { return node == Op::Num; }
    constexpr bool operator==(const Slot&) const = default;
};

/// A side condition of a rule beyond what its slots state.
enum class Cond : u8 {
    None,
    Fold, ///< The root has a memory operand form and sits at its inner load, so its holes must be available there.
    Rmw,  ///< The inner load reads the root store's address on the memory the store takes, which nothing else uses.
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
};

struct Rule;

/// An alternative of a PBQP node: a match rooted there, or the class of all matches covering it as the same inner node.
struct Alt {
    const Rule* rule = nullptr; ///< `nullptr` for an inner alternative.
    size_t inner     = 0;
    pbqp::Cost cost  = 0;
    fe::Vector<Demand> demands;
    fe::Vector<Cover> covers;
};

using Alts = fe::Vector<Alt>;

/// Builds the replacement of the match @p alt rooted at the old_world() Def @p root.
using BuildFn = const Def* (ISel::*)(const Def*, const Alt&);

/// One pattern ↦ replacement: the root's operation, the pattern's slots, the replacement's type, cost, and builder.
struct Rule {
    static constexpr size_t Max_Slots = 6;

    Op op;
    Ty type;
    pbqp::Cost cost;
    std::string_view name;
    BuildFn build;
    Cond cond    = Cond::None;
    u8 num_slots = 0;
    std::array<Slot, Max_Slots> slots{};

    constexpr Rule with(Cond c) const {
        auto res = *this;
        res.cond = c;
        return res;
    }
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
    /// A binary x86 instruction builder.
    using Insn = const Def* (*)(World&, const Def*, const Def*);

    /// Applies @p insn, going through `i32` for operands narrower than 16 bits.
    const Def* wide(Insn insn, const Def* type, bool sign, const Def* a, const Def* b);

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
    /// An operation of old_world() as a pattern node: its `Op`, its operands, and whether they commute.
    struct Node {
        Op op;
        DefVec ops;
        bool commutes = false;
    };
    /// The operands of one pattern node that a match has yet to bind.
    struct Frame {
        const Def* node;
        DefVec ops;
        size_t next  = 0;
        size_t cover = size_t(-1); ///< The Cover whose demands they are, or `size_t(-1)` for the root.
    };

    /// @p def as a pattern node, if a pattern covers it.
    std::optional<Node> decompose(const Def* def) const;
    /// Appends the matches rooted at @p def to @p alts.
    void match(const Def* def, Alts& alts) const;
    /// Appends the matches of @p rule rooted at @p root, which decomposes to @p node, to @p alts.
    void append(const Rule& rule, const Def* root, const Node& node, Alts& alts) const;
    /// Binds the slots of @p rule from @p i on to the pending operands in @p frames; complete matches go to @p alts.
    void bind(const Rule& rule, const Def* root, size_t i, Alt alt, fe::Vector<Frame> frames, Alts& alts) const;
    /// The inner node the operand @p def of @p parent stands for, if nothing outside the match uses it.
    const App* sole(const Frame& parent, const Def* def) const;
    /// Does the match @p alt rooted at @p root meet the side condition of its rule?
    bool holds(const Def* root, const Alt& alt) const;
    /// The index of the first atomic alternative of @p alts, or `size_t(-1)`.
    size_t atomic_choice(const Alts& alts) const;
#ifdef MIM_ENABLE_CHECKS
    /// Verifies atomic completeness, total conversions, and inner-node coverage over the rule set.
    void check_rules() const;
#endif
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
