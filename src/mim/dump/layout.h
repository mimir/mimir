#pragma once

#include <deque>
#include <span>
#include <vector>

#include "mim/def.h"
#include "mim/lam.h"
#include "mim/nest.h"

namespace mim {
class Match;
class Scheduler;
} // namespace mim

namespace mim::dump {

class Names;
struct Binder;

/// A mutable the dump declares by name; every other Def prints where it is used.
Def* isa_decl(const Def*);
/// Def::num_tprojs read off the structure of @p type: the frozen World need not hold the arity's Lit.
nat_t num_binders(const Def* type);
/// Does @p def print with its operands wherever it occurs - instead of by the name a `let` binds?
bool spells_inline(const Def*);
/// What a Hole stands for: its solution, followed read-only - Hole::find would path-compress.
const Def* solve(const Def*);

/// One binder position: a Var, one of its projections, or a projection's projection.
struct Slot {
    const Def* def  = nullptr; ///< The Var or Extract - if the frozen World holds it.
    const Def* type = nullptr;
    Binder* owner   = nullptr; ///< Set on a root.
    mutable Sym sym;           ///< Picked when first printed; see Layout::slot_name.
    bool used     = false;     ///< Referenced as a whole - not merely through its projections.
    bool field    = false;     ///< Referenced through its Sigma's own Var; names the field, destructures nothing.
    bool ret      = false;     ///< A `fun`'s `return`.
    bool expanded = false;
    bool pattern  = false;   ///< Spelled as a tuple pattern; whole otherwise.
    std::vector<Slot> comps; ///< std::vector: fe::Vector needs a complete element type.

    bool is_tuple() const { return !comps.empty(); }
};

/// The binder a Var belongs to; a Lam stands in for its Pi and its anonymous dependent dom Sigma.
struct Binder {
    enum class Kind : u8 { None, Lam, Con, Fun };

    Def* mut;
    Slot root;
    fe::Vector<const Var*> vars; ///< The Var%s the root stands for.
    Def* scope = nullptr;        ///< The closed decl this binder sits in.
};

struct Item {
    enum class Tag : u8 { Let, Decl, Axms };

    Tag tag;
    const Def* def = nullptr;    ///< Let.
    fe::Vector<Def*> muts;       ///< Decl: one SCC, an `and` chain if several.
    fe::Vector<const Axm*> axms; ///< Axms: the Axm%s of one `mod`.
    Sym mod;                     ///< Axms; empty for bare `axm`s.
    bool rec = false;            ///< Decl: a non-Lam member refers to itself.
};

struct Block {
    Lam* owner; ///< `nullptr`: the top level.
    Block* parent;
    fe::Vector<Item> items;
    const Def* tail = nullptr;
};

/// What the dump decided before anything prints: which Def is declared or `let`-bound where, how every binder is
/// spelled, and what everything is called. Dump::Expr runs no analysis; its queries answer structurally.
class Layout {
public:
    using Kind = Binder::Kind;

    struct Opts {
        Def::Dump mode;
        bool typed_let                           = false;
        absl::flat_hash_set<const fe::Src*> skip = {}; ///< Files an `import` declares: their externals stay theirs.
        std::string self                         = {}; ///< `<file stem>.` - the prefix of the file's own annexes.
    };

    Layout(World&, Opts);

    World& world() const { return world_; }
    const Opts& opts() const { return opts_; }

    /// @name seeding
    ///@{
    void add_axms(std::span<const std::pair<std::string, const Axm*>>); ///< Before Layout::add_root.
    void add_root(const Def*);
    void finish(Names&);
    ///@}

    /// @name queries - pure after Layout::finish
    ///@{
    const Block& top() const { return top_; }
    const Block* block(Lam* head) const; ///< `nullptr`: @p head is a λ-expression.
    const Binder* binder(Def* mut) const;
    /// The Slot naming a Var or a projection of one; within the header of @p ctx, that Lam's Pi and dom Sigma Var%s
    /// stand for its own.
    const Slot* resolve(const Def*, const Lam* ctx = nullptr) const;
    /// Empty if @p def is spelled out where it is used. Names are picked in printing order, so only what prints
    /// ever takes one - and a re-read, printed in the same order, picks the same ones again.
    Sym name(const Def*, const Lam* ctx = nullptr) const;
    /// The Slot's name - if it is used, or a field or a user-named projection spelled whole; empty for `_`.
    Sym slot_name(const Slot&) const;
    bool in_header(const Def* def) const { return header_.contains(def); }
    bool is_arm(const Def* def) const { return arms_.contains(def); }
    bool spellable(const Match*) const;
    bool unspellable(const Def* def) const { return unspell_.contains(def); }
    Kind kind(const Lam*) const;
    fe::Vector<Lam*> chain(Lam*) const; ///< The curried chain @p lam heads - or `{lam}`.
    ///@}

private:
    template<class F>
    void deps(const Def*, F&&) const;
    bool descends(const Def* mut) const;
    bool is_seed(const Def* def) const { return std::ranges::contains(seeds_, def); }
    static bool fun_shape(const Lam*);
    static const Def* binder_type(const Def* mut);
    static const Def* comp_type(const Def* type, size_t i);
    static bool user_named(const Def*);

    void reach1(const Def*);
    void classify();
    void reach3(const Def*, Def* curr, Def* scope, bool inl);
    void headers();
    void place();
    void place_local();
    void header_rule(Lam* head, Block*, const Scheduler*);
    void order();
    void tarjan(Block&, const DefVec& nodes);
    Item axms_item(size_t group) const;
    void spell();

    const Var* find(const Var*) const;
    void unite(const Var* var, const Var* rep);
    static bool is_ctx_var(const Var*, const Lam* ctx);
    bool last_in_chain(const Lam*) const;
    Binder* ensure_binder(Def* mut);
    Slot* resolve_(const Def*, const Lam* ctx = nullptr);
    void expand(Slot&);
    void mark(const Def* user, const Def* dep);
    Block* blk(const Nest::Node*);
    Block* block_of(Lam* head);
    static bool at_or_below(const Block*, const Block*);
    const DefSet& refs(const Def* item);
    void walk(const Def*, DefSet& out, DefSet& seen, const Def* self, bool& recursive);
    bool is_item(const Def*) const;
    bool decide(Slot&);
    Lam* head_of(Lam*) const;
    const Binder* structural(Def* mut) const;
    void expand_structural(Slot&, bool force) const;

    World& world_;
    Opts opts_;
    DefVec seeds_, roots_;
    fe::Vector<std::pair<Sym, fe::Vector<const Axm*>>> axm_groups_;
    DefMap<size_t> axm2group_; ///< Own Axm → its group; the group's first Axm keys the pseudo item.
    DefSet group_keys_;
    // P1
    DefSet reached_, descended_;
    DefVec order1_;
    DefMap<u32> uses_;
    // P2
    DefSet arms_, matches_, decls_;
    DefMap<fe::Vector<Lam*>> chains_;
    DefMap<Lam*> head_;
    DefMap<const Var*> alias_;
    DefMap<fe::Vector<Lam*>> alias_lams_;       ///< A shared Pi's or dom Sigma's Var → the Lam%s it stands in for.
    DefMap<fe::Vector<const Var*>> extra_vars_; ///< Lam → such shared Var%s.
    fe::Vector<Def*> scopes_, root_heads_;
    // P3
    DefSet pre3_;
    DefMap<u32> seq_;
    DefVec order3_;
    DefMap<Def*> curr_;
    DefMap<DefVec> cands_;
    DefSet cand_set_;
    DefMap<fe::Vector<Def*>> open_decls_;
    fe::Vector<Lam*> heads_;
    DefMap<fe::Vector<Lam*>> scope_heads_;
    std::deque<Binder> binder_store_;
    DefMap<Binder*> binders_;
    DefMap<DefSet> hdr_;
    DefMap<fe::Vector<Def*>> hdr_decls_;
    fe::Vector<std::pair<const Def*, Binder*>> type_work_; ///< Projection types the frozen World hands out.
    // P4
    DefMap<Block*> place_, decl_block_;
    DefSet header_, unspell_;
    Block top_;
    std::deque<Block> blocks_;
    DefMap<Block*> block_idx_;
    // P5
    DefMap<DefSet> refs_;
    DefSet self_rec_;
    // P6
    DefMap<Kind> kinds_;
    // P7: lazily, in printing order
    Names* names_ = nullptr;
    mutable DefMap<Sym> picked_;
    // Dump::Expr
    mutable std::deque<Binder> structural_store_;
    mutable DefMap<Binder*> structural_;
    mutable DefMap<Kind> structural_kinds_;
};

} // namespace mim::dump
