#pragma once

#include <memory>

#include <fe/assert.h>
#include <fe/cast.h>

#include "mim/def.h"
#include "mim/nest.h"
#include "mim/rewrite.h"
#include "mim/world.h"

namespace mim {

class Nest;
class Phase;
class PhaseMan;
class World;

using Phases = std::deque<std::unique_ptr<Phase>>;

/// A Phase performs one self-contained task over the whole World.
/// Phases are intended to run in a classical sequence, one after another.
/// @see @ref phases_phase
class Phase : public fe::RuntimeCast<Phase> {
public:
    /// @name Construction & Destruction
    ///@{
    Phase(World& world, std::string name)
        : world_(world)
        , name_(std::move(name)) {}
    Phase(World& world, flags_t annex);

    /// Clears the dirty bits of all muts still recorded in dirty(): otherwise a discarded Phase
    /// (e.g. one recreated by a fixed-point PhaseMan) would strand set bits without a list entry,
    /// causing later taint() calls to silently skip recording those muts.
    virtual ~Phase() {
        for (auto mut : dirty_)
            mut->dirty(false);
    }

    virtual std::unique_ptr<Phase> recreate(); ///< Creates a new instance; needed by a fixed-point PhaseMan.
    virtual void apply(const App*) {}          ///< Invoked if your Phase has additional args.
    virtual void apply(Phase&) {}              ///< Dito, but invoked by Phase::recreate.

    /// @name Redirection
    /// A Phase may resolve to a *different* Phase (or to nothing) after Phase::apply.
    /// This is used by the `%%compile.named` stage that resolves a string to another plugin's annex.
    ///@{
    virtual bool redirects() const { return false; } ///< If `true`, Phase::create uses take_resolved().
    virtual std::unique_ptr<Phase> take_resolved() {
        return {};
    } ///< The Phase to use instead; `nullptr` means *elide*.
    ///@}

    static std::unique_ptr<Phase> create(const Flags2Phases& phases, const Def* def) {
        auto& world = def->world();
        auto p_def  = App::uncurry_callee(def);
        world.DLOG("apply phase: `{}`", p_def);

        if (auto axm = p_def->isa<Axm>())
            if (auto i = phases.find(axm->flags()); i != phases.end()) {
                auto phase = i->second(world);
                if (phase) {
                    phase->apply(def->isa<App>());
                    if (phase->redirects()) return phase->take_resolved();
                }
                return phase;
            } else
                error("phase `{}` not found", axm->sym());
        else
            error("unsupported callee for a phase: `{}`", p_def);
    }

    template<class A, class P>
    static void hook(Flags2Phases& phases) {
        assert_emplace(phases, Annex::base<A>(), [](World& w) { return std::make_unique<P>(w, Annex::base<A>()); });
    }
    ///@}

    /// @name Getters
    ///@{
    World& world() { return world_; }
    Driver& driver() { return world().driver(); }
    Log& log() const { return world_.log(); }
    std::string_view name() const { return name_; }
    flags_t annex() const { return annex_; }
    ///@}

    /// @name Fixed-Point Handling
    ///@{
    bool todo() const { return todo_; }

    /// Signals that another round of fixed-point iteration is required, either
    /// as part of
    /// - a pipeline managed by PhaseMan, or
    /// - the optional pre-analysis of an RWPhase.
    ///
    /// Calling `invalidate(todo)` bitwise-ORs @p todo into the internal `todo_` flag.
    void invalidate(bool todo = true) { todo_ |= todo; }

    /// The muts this Phase changed or wants re-examined - the unified "dirt" currency:
    /// a sparse Analysis seeds its next round from it, an RWPhase records its rewrite sites in it
    /// (and clears it again before its world swap - recorded Def%s must never outlive their World).
    /// Insertion-ordered and deduplicated via Def%'s *dirty bit*.
    const auto& dirty() const { return dirty_; }

    /// Marks @p mut as *needs a (re)visit*: sets Def::is_dirty() and records @p mut in dirty().
    /// Whoever visits @p mut clears the bit again; a recorded mut whose bit is already clear is stale
    /// (it got its visit in the meantime) and is simply skipped by consumers.
    void taint(Def* mut) {
        if (mut && !mut->is_dirty()) {
            mut->dirty();
            dirty_.emplace_back(mut);
        }
    }
    ///@}

    /// @name run
    ///@{
    virtual void run();       ///< Entry point and generates some debug output; invokes Phase::start.
    virtual void start() = 0; ///< Actual entry.

    /// Runs a single Phase.
    template<class P, class... Args>
    static void run(Args&&... args) {
        P p(std::forward<Args>(args)...);
        p.run();
    }
    ///@}

private:
    World& world_;
    flags_t annex_ = 0;
    bool todo_     = false;

protected:
    std::string name_;
    Vector<Def*> dirty_;

private:
    friend class Analysis;
};

/// Traverses the current World using Rewriter infrastructure while staying in the same world.
///
/// It recursively rewrites
/// 1. all World::annexes() (during which Analysis::is_bootstrapping() is `true`), and then
/// 2. all World::externals() (during which it is `false`).
///
/// Analysis provides a reusable lattice() mapping old Def%s to abstract values, represented as ordinary MimIR Def%s.
/// @note You can override
/// - Rewriter::rewrite(),
/// - Rewriter::rewrite_imm(),
/// - Rewriter::rewrite_mut(), etc.
/// @see @ref phases_analysis
class Analysis : public Phase, public Rewriter {
public:
    /// @name Construction & Destruction
    ///@{
    Analysis(World& world, std::string name)
        : Phase(world, std::move(name))
        , Rewriter(world) {}
    Analysis(World& world, flags_t annex)
        : Phase(world, annex)
        , Rewriter(world) {}

    /// Clears the rewriter map and resets Phase::todo() for the next fixed-point iteration.
    /// lattice() is **preserved** across iterations so that abstract values accumulated in earlier
    /// rounds remain available - this is what makes fixed-point convergence possible.
    /// @see RWPhase::analyze
    virtual void reset();
    ///@}

    /// @name Getters
    ///@{
    World& world() { return Phase::world(); }
    bool is_bootstrapping() const { return bootstrapping_; }
    ///@}

    /// @name Sparse Fixed-Point Iteration
    /// By default, every fixed-point round re-traverses the whole World.
    /// A *sparse* Analysis instead re-drains only the mutables *tainted* by lattice changes:
    /// whenever update()/set() changes an entry, the muts that read that entry (tracked automatically
    /// during draining) plus the entry's owner() are scheduled for the next round.
    /// Once sparse rounds quiesce, one **full** round certifies the fixed point
    /// (and is the only kind of round that runs finalize()) - so untracked flows through
    /// side tables or post-passes cannot be missed.
    ///@{
    bool sparse() const { return sparse_; }
    void make_sparse() { sparse_ = true; }
    ///@}

    /// @name lattice
    /// Conventions: *absent* = ⊥ (nothing known); `def ↦ def` = ⊤ (keep as is).
    /// Subclasses may store their own sentinels in between as ordinary Def%s (e.g. SEO's GVN-bundle Proxy%s).
    ///@{
    const auto& lattice() const { return lattice_; }

    /// @returns the abstract value recorded for @p def, or `nullptr` if unknown.
    /// While draining a sparse() Analysis, the lookup registers curr_mut() as a *reader* of @p def,
    /// so a later change to this entry taint()s the mut for re-visiting.
    const Def* lattice(const Def* def) const {
        if (tracking_)
            if (auto mut = curr_mut()) readers_[def].emplace(mut);
        if (auto i = lattice_.find(def); i != lattice_.end()) return i->second;
        return nullptr;
    }

    bool is_top(const Def* def) const { return lattice(def) == def; }

    /// Records the abstract value @p abstr for @p concr in both lattice() (the analysis result)
    /// and map() (so the rewriter short-circuits future rewrites of @p concr to @p abstr).
    /// Bypasses update() and hence never invalidate()s - but still taint()s on change for sparse() mode.
    const Def* set(const Def* concr, const Def* abstr) {
        if (auto [i, ins] = lattice_.emplace(concr, abstr); !ins && i->second != abstr) {
            i->second = abstr;
            taint(concr);
        }
        // A sparse round replays these pairs: a dirty mut's rewrite must see the substitutions
        // its (possibly non-dirty) producers would have re-installed in a full round.
        if (sparse_) set_map_[concr] = abstr;
        return map(concr, abstr);
    }

    /// Monotonically forces @p def to ⊤ (keep as is).
    const Def* pin_top(const Def* def) {
        update(def, def);
        return def;
    }
    ///@}

protected:
    /// @name lattice
    ///@{

    /// Writes `concr ↦ abstr` into lattice(); does **not** map().
    /// invalidate()s - and thereby triggers another fixed-point round - iff this changes observable information:
    /// an existing entry was overwritten, or a fresh fact other than ⊤ was inserted.
    /// Freshly inserting ⊤ (`concr ↦ concr`) stays silent, as it is indistinguishable from *absent* for consumers.
    /// @returns the former abstract value:
    /// - `nullptr`: fresh insert,
    /// - `== abstr`: unchanged,
    /// - anything else: overwritten.
    const Def* update(const Def* concr, const Def* abstr) {
        auto [i, ins] = lattice_.emplace(concr, abstr);
        if (ins) {
            if (concr != abstr) {
                invalidate();
                taint(concr);
            }
            return nullptr;
        }

        auto old = i->second;
        assert((old != concr || abstr == concr) && "monotonicity violation: must not descend from ⊤");
        if (old != abstr) {
            i->second = abstr;
            invalidate();
            taint(concr);
        }
        return old;
    }

    using Phase::taint;

    /// Schedules all muts affected by a change to @p key for the next sparse round:
    /// its recorded readers plus its owner(). No-op for a dense Analysis.
    void taint(const Def* key) {
        if (!sparse_) return;
        taint(owner(key));
        if (auto i = readers_.find(key); i != readers_.end())
            for (auto mut : i->second)
                taint(mut);
    }

    /// The mut whose body consumes the abstract value of @p key - dirtied alongside the readers.
    /// Default handles (projections of) a Var; override for subclass-specific keys (e.g. SEO's phi proxies).
    virtual Def* owner(const Def* key) {
        if (auto var = key->isa<Var>()) return var->mut();
        if (auto ex = key->isa<Extract>())
            if (auto var = ex->tuple()->isa<Var>()) return var->mut();
        return nullptr;
    }
    ///@}

    /// @name Rewrite
    ///@{
    virtual void prepare() {}  ///< Run **before** the main analysis.
    virtual void finalize() {} ///< Run **after** the main analysis.
    void start() override;
    virtual void rewrite_annex(flags_t, Sym, const Def*);
    virtual void rewrite_external(Def*);

    /// Schedules @p mut for a breadth-first visit of its dependencies and records `mut -> mut`.
    /// Mutables are enqueued instead of recursed into; Analysis::drain then walks them in BFS order.
    /// The `mut -> mut` entry doubles as the per-round "already scheduled" marker (Rewriter::old2news_ is
    /// cleared by reset()), so each mutable's deps are visited at most once per fixed-point round.
    Def* rewrite_mut(Def*) override;
    ///@}

private:
    /// Walks all enqueued mutables' dependencies - in BFS order - under each mutable's curr_mut() scope.
    void drain();

    Def2Def lattice_;
    std::deque<Def*> worklist_;
    bool bootstrapping_ = true;

    // sparse fixed-point iteration; Phase::dirty_ holds the muts to re-drain next round (filled by taint())
    /// lattice key -> muts whose visit read it
    mutable absl::node_hash_map<const Def*, MutSet, GIDHash<const Def*>> readers_;
    Def2Def set_map_;         ///< all set() pairs; replayed into map() at sparse-round start
    Vector<Def*> dirty_prev_; ///< the muts being re-drained this round
    bool sparse_     = false;
    bool tracking_   = false; ///< record readers_ only while draining
    bool full_round_ = true;  ///< current round traverses the whole World
    bool need_full_  = false; ///< sparse rounds quiesced; certify with a full round
    uint32_t round_  = 0;
};

/// Rebuilds old_world() into new_world() and then swaps them.
///
/// It recursively rewrites
/// 1. all old World::annexes() (during which RWPhase::is_bootstrapping() is `true`, and then
/// 2. all old World::externals() (during which it is `false`).
///
/// During bootstrapping, rewrites that depend on other annexes may need to be skipped,
/// since those annexes might not yet exist in the new world.
///
/// If an associated Analysis is provided, the rewrite can query its abstract results through lattice().
///
/// @note You can override
/// - Rewriter::rewrite(),
/// - Rewriter::rewrite_imm(),
/// - Rewriter::rewrite_mut(), etc.
/// @see @ref phases_rwphase
class RWPhase : public Phase, public Rewriter {
public:
    /// @name Construction
    ///@{
    RWPhase(World& world, std::string name, Analysis* analysis = nullptr)
        : Phase(world, std::move(name))
        , Rewriter(world.inherit())
        , analysis_(analysis) {}
    RWPhase(World& world, flags_t annex, Analysis* analysis = nullptr)
        : Phase(world, annex)
        , Rewriter(world.inherit())
        , analysis_(analysis) {}
    ///@}

    /// @name Analysis
    ///@{
    Analysis* analysis() { return analysis_; }
    const Analysis* analysis() const { return analysis_; }

    /// Returns the abstract value computed by the associated Analysis for the given old-world Def, or `nullptr` if no
    /// value is available.
    const Def* lattice(const Def* old_def) { return analysis_ ? analysis_->lattice(old_def) : nullptr; }

    /// Returns lattice(@p old_def) if it differs from @p old_def (i.e. we learned something), otherwise `nullptr`.
    const Def* abstracted(const Def* old_def) {
        auto l = lattice(old_def);
        return l && l != old_def ? l : nullptr;
    }

    /// Runs the optional pre-analysis on RWPhase::old_world(), typically to a fixed point,
    /// before rewriting begins.
    ///
    /// If analysis() is set, this is the natural place to iterate until Phase::todo() becomes `false`.
    /// If no Analysis is needed, simply return `false`.
    virtual bool analyze();
    ///@}

    /// @name Rewrite
    ///@{
    virtual void rewrite_annex(flags_t, Sym, const Def*);
    virtual void rewrite_external(Def*);

    /// Returns whether we are currently bootstrapping (rewriting annexes).
    /// While bootstrapping, you have to skip rewrites that refer to other annexes, as they might not yet be available.
    bool is_bootstrapping() const { return bootstrapping_; }
    ///@}

    /// @name Fixed-Point Handling
    ///@{

    /// Like Phase::invalidate but additionally taint()s curr_mut(), recording *where* the change happened.
    /// RWPhase::start translates the collected dirt into the new world upon swap.
    /// @note This deliberately **hides** the non-virtual Phase::invalidate: rewrite hooks call it from RWPhase
    /// context, so dispatch is static and inlinable - no virtual call in the hot rewrite path.
    /// A call through a `Phase*` skips the tainting, which is harmless: dirt is best-effort metadata.
    void invalidate(bool todo = true) {
        if (todo) taint(curr_mut());
        Phase::invalidate(todo);
    }
    ///@}

    /// @name World
    /// * Phase::world is the **old** one.
    /// * Rewriter::world is the **new** one.
    /// * RWPhase::world is deleted to not confuse this.
    ///@{
    using Phase::world;
    using Rewriter::world;
    World& world() = delete;                         ///< Hides both and forbids direct access.
    World& old_world() { return Phase::world(); }    ///< Get **old** Def%s from here.
    World& new_world() { return Rewriter::world(); } ///< Create **new** Def%s into this.
    ///@}

protected:
    void start() override;

private:
    Analysis* analysis_;
    bool bootstrapping_ = true;
};

/// An RWPhase that searches for a pattern and replaces it.
/// Implement the replace() hook - or use the MIM_REPL macro for an inline definition.
class Repl : public RWPhase {
public:
    Repl(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    /// replace() inspects and builds Def%s of the **old** world; the RWPhase machinery carries the result over.
    World& world() { return old_world(); }

    /// @returns the replacement or `nullptr` if the pattern does not match.
    virtual const Def* replace(const Def* def) = 0;

private:
    const Def* rewrite(const Def* def) final {
        for (bool todo = true; todo;) {
            todo = false;
            if (auto subst = replace(def)) todo = true, def = subst;
        }

        return Rewriter::rewrite(def);
    }
};

#define MIM_CONCAT_INNER(a, b) a##b
#define MIM_CONCAT(a, b)       MIM_CONCAT_INNER(a, b)

#define MIM_REPL(__phases, __annex, ...) MIM_REPL_IMPL(__phases, __annex, __LINE__, __VA_ARGS__)

// clang-format off
#define MIM_REPL_IMPL(__phases, __annex, __id, ...)                         \
    struct MIM_CONCAT(Repl_, __id) : ::mim::Repl {                          \
        MIM_CONCAT(Repl_, __id)(::mim::World & world, ::mim::flags_t annex) \
            : Repl(world, annex) {}                                         \
                                                                            \
        const ::mim::Def* replace(const ::mim::Def* def) final __VA_ARGS__  \
    };                                                                      \
    ::mim::Phase::hook<__annex, MIM_CONCAT(Repl_, __id)>(__phases)
// clang-format on

/// Removes unreachable and dead code by rebuilding the whole World into a new one and `swap`ping them afterwards.
/// @see @ref phases_rwphase
class Cleanup : public RWPhase {
public:
    Cleanup(World& world)
        : RWPhase(world, "cleanup") {}
    Cleanup(World& world, flags_t annex)
        : RWPhase(world, annex) {}
};

/// Organizes several Phase%s into a pipeline.
/// If fixed_point() is `true`, rerun the whole pipeline until all Phase::todo()%s flags remain `false`.
/// @see @ref phases_phase_man
class PhaseMan : public Phase {
public:
    /// @name Construction
    ///@{
    PhaseMan(World& world, flags_t annex)
        : Phase(world, annex) {}

    void apply(bool, Phases&&);
    void apply(const App*) final;
    void apply(Phase&) final;
    ///@}

    /// @name Getters
    ///@{
    bool fixed_point() const { return fixed_point_; }
    auto& phases() { return phases_; }
    const auto& phases() const { return phases_; }
    ///@}

private:
    void start() final;

    Phases phases_;
    bool fixed_point_;
};

/// Transitively visits all *reachable*, [*closed*](@ref Def::is_closed) mutables in the World.
/// * Select with `elide_empty` whether you want to visit trivial mutables without body.
/// * Set `schedule` if the mutables should be scheduled to ensure a correct order of dependencies.
/// * If you are only interested in specific mutables, you can pass this to @p M.
/// @see @ref phases_closed_mut_phase
template<class M = Def>
class ClosedMutPhase : public Phase {
public:
    ClosedMutPhase(World& world, std::string name, bool elide_empty, bool schedule = false)
        : Phase(world, std::move(name))
        , elide_empty_(elide_empty)
        , schedule_(schedule) {}
    ClosedMutPhase(World& world, flags_t annex, bool elide_empty, bool schedule = false)
        : Phase(world, annex)
        , elide_empty_(elide_empty)
        , schedule_(schedule) {}

    bool elide_empty() const { return elide_empty_; }
    bool schedule() const { return schedule_; }

protected:
    void start() override {
        world().template for_each<M>(elide_empty(), [this](M* mut) { root_ = mut, visit(mut); }, schedule());
    }
    virtual void visit(M*) = 0;
    M* root() const { return root_; }

private:
    const bool elide_empty_;
    const bool schedule_;
    M* root_;
};

/// Like ClosedMutPhase but computes a Nest for each NestPhase::visit.
/// @see @ref phases_nest_phase
template<class M = Def>
class NestPhase : public ClosedMutPhase<M> {
public:
    NestPhase(World& world, std::string name, bool elide_empty, bool schedule = false)
        : ClosedMutPhase<M>(world, std::move(name), elide_empty, schedule) {}
    NestPhase(World& world, flags_t annex, bool elide_empty, bool schedule = false)
        : ClosedMutPhase<M>(world, annex, elide_empty, schedule) {}

    const Nest& nest() const { return *nest_; }
    virtual void visit(const Nest&) = 0;

private:
    void visit(M* mut) final {
        Nest nest(mut);
        nest_ = &nest;
        visit(nest);
    }

    const Nest* nest_;
};

} // namespace mim
