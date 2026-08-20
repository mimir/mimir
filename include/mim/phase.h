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

    virtual ~Phase() = default;

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
                fe::throwf("phase `{}` not found", axm->sym());
        else
            fe::throwf("unsupported callee for a phase: `{}`", p_def);
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

    /// Command-line arguments passed to this Phase's plugin via `-X <plugin>:<arg>`.
    /// Derived from Phase::annex; yields an empty Vector for name-constructed Phase%s.
    const Vector<std::string>& args();
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

    /// Adds @p n to the custom Profiler counter @p key of the current run; no-op unless profiling is enabled.
    void profile_count(std::string_view key, uint64_t n = 1);
    ///@}

private:
    World& world_;
    flags_t annex_ = 0;
    bool todo_     = false;

protected:
    std::string name_;

    friend class Analysis;
};

/// Traverses the current World using Rewriter infrastructure while staying in the same world.
///
/// It recursively rewrites
/// 1. all World::annexes() (during which Analysis::is_bootstrapping() is `true`), and then
/// 2. all World::externals() (during which it is `false`).
///
/// Analysis provides a reusable lattice() mapping old Def%s to abstract values, represented as ordinary MimIR Def%s.
///
/// Fixed-point iteration is *sparse* by default:
/// whenever a lattice write changes observable information, the mutable currently being drained is recorded as *dirty*.
/// The next round then re-drains only those dirty mutables - plus everything reachable from them - instead of walking
/// the whole World. At the start of such a round, the accumulated lattice() is replayed into the rewriter map, so a
/// dirty mutable's body sees the substitutions its (non-revisited) producers installed in earlier rounds. Since dirt
/// tracks *writers* - not readers - a sparse round may miss affected mutables; hence, once sparse rounds quiesce, one
/// final **full** round certifies the fixed point.
/// @note You can override
/// - Rewriter::rewrite(),
/// - Rewriter::rewrite_imm(),
/// - Rewriter::rewrite_mut(), etc.
/// @see @ref phases_analysis
/// @see @ref ssa-without-dominance for how an Analysis substitutes Def::nests for the classical SSA dominance check.
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
    /// The dirty set survives as well; start() consumes it to decide whether the round can be sparse.
    /// @see RWPhase::analyze
    virtual void reset();
    ///@}

    /// @name Getters
    ///@{
    using Phase::world; ///< Disambiguates the Phase/Rewriter double base; for an Analysis both denote the same World.
    bool is_bootstrapping() const { return bootstrapping_; }
    ///@}

    /// @name Sparse Fixed-Point Iteration
    ///@{
    bool is_sparse() const {
        return curr_sparse_;
    } ///< Does the current round only re-drain last round's dirty mutables?
    void make_dense() { dense_ = true; } ///< Forces whole-World rounds unconditionally.
    /// Bumped on every observable lattice change; snapshot it around a code region to detect changes.
    size_t version() const { return version_; }
    ///@}

    /// @name lattice
    /// Conventions: *absent* = ⊥ (nothing known); `def ↦ def` = ⊤ (keep as is).
    /// Subclasses may store their own sentinels in between as ordinary Def%s (e.g. SEO's GVN-bundle Proxy%s).
    ///@{
    const auto& lattice() const { return lattice_; } ///< The whole map; used e.g. to diff two fixed-point runs.

    /// @returns the abstract value recorded for @p def, or `nullptr` if unknown.
    const Def* lattice(const Def* def) const {
        if (auto i = lattice_.find(def); i != lattice_.end()) return i->second;
        return nullptr;
    }

    /// @returns whether @p def is pinned to ⊤ (`def ↦ def`).
    bool is_top(const Def* def) const {
        auto i = lattice_.find(def);
        return i != lattice_.end() && i->second == def;
    }
    ///@}

protected:
    /// @name lattice
    ///@{

    /// **Non-monotone** write of `concr ↦ abstr` into lattice() and map().
    /// This is the escape hatch for analyses that must overwrite an earlier round's value (descending from ⊤ is fine).
    /// invalidate()s iff the stored value changed; an *absent* entry counts as changed - even for ⊤ -
    /// since a non-monotone lattice's consumers may well distinguish ⊥ from ⊤.
    /// Every change also touch()es curr_mut() - the seed set of the next sparse round.
    /// @returns `true` iff this changed the entry - i.e. iff it invalidate()d.
    bool lattice_force(const Def* concr, const Def* abstr) {
        map(concr, abstr);

        if (auto [i, ins] = lattice_.emplace(concr, abstr); !ins) {
            if (i->second == abstr) return false;
            i->second = abstr;
        }
        return touch(), true;
    }

    /// Writes `concr ↦ abstr` into lattice() and map().
    /// invalidate()s - and thereby triggers another fixed-point round - iff this changes observable information:
    /// an existing entry was overwritten, or a fresh fact other than ⊤ was inserted.
    /// Freshly inserting ⊤ (`concr ↦ concr`) stays silent, as it is indistinguishable from *absent* for consumers.
    /// Every change also touch()es curr_mut() - the seed set of the next sparse round.
    /// @returns `true` iff this changed observable information - i.e. iff it invalidate()d.
    bool lattice(const Def* concr, const Def* abstr) {
        map(concr, abstr);

        if (auto [i, ins] = lattice_.emplace(concr, abstr); !ins) {
            assert((i->second != concr || abstr == concr) && "monotonicity violation: must not descend from ⊤");
            if (i->second == abstr) return false;
            i->second = abstr;
        } else if (concr == abstr) {
            return false;
        }
        return touch(), true;
    }

    /// Monotonically forces @p def to ⊤ (keep as is).
    /// @returns `true` iff this changed observable information - i.e. iff it invalidate()d.
    bool pin(const Def* def) { return lattice(def, def); }

    /// Additionally schedules @p mut for the next sparse round.
    /// Use this when a lattice change must re-visit *other* mutables than curr_mut() -
    /// e.g. all call sites of a Lam whose var's abstract value changed.
    void taint(Def* mut) { dirty_.emplace(mut); }
    ///@}

    /// @name Rewrite
    ///@{
    virtual void prepare() {} ///< Run **before** the main analysis.
    /// Run **after** the main analysis - only in **full** rounds, so it always sees the complete abstract World.
    virtual void finalize() {}
    void start() override;
    virtual void rewrite_annex(flags_t, Sym, const Def*);
    virtual void rewrite_external(Def*);
    const Def* rewrite_imm_Proxy(const Proxy* proxy) override { return proxy; } ///< By default: ignore Proxy%s.

    /// Schedules @p mut for a breadth-first visit of its dependencies and records `mut -> mut`.
    /// Mutables are enqueued instead of recursed into; Analysis::drain then walks them in BFS order.
    /// The `mut -> mut` entry doubles as the per-round "already scheduled" marker (Rewriter::old2news_ is
    /// cleared by reset()), so each mutable's deps are visited at most once per fixed-point round.
    Def* rewrite_mut(Def*) override;
    ///@}

private:
    /// Observable lattice information changed: records curr_mut() as *dirty* - the seed set of the next sparse
    /// round - and invalidate()s. Outside of any mutable (annex walk, finalize()) the change cannot be
    /// attributed to a mutable; then the next round falls back to a full one.
    void touch() {
        ++version_;
        if (auto mut = curr_mut())
            dirty_.emplace(mut);
        else
            nonlocal_ = true;
        invalidate();
    }

    /// Walks all enqueued mutables' dependencies - in BFS order - under each mutable's curr_mut() scope.
    void drain();

    Def2Def lattice_;
    std::deque<Def*> worklist_;
    MutSet dirty_;               ///< Muts whose drain changed the lattice this round; seeds the next sparse round.
    size_t version_     = 0;     ///< @see version()
    bool nonlocal_      = false; ///< The lattice changed outside of any mut; the next round must be a full one.
    bool curr_sparse_   = false; ///< Is the current round sparse?
    bool dense_         = false; ///< @see make_dense()
    bool bootstrapping_ = true;
    size_t num_drained_ = 0; ///< muts drained this round; flushed into the Profiler
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
    const Def* lattice(const Def* old_def) const { return analysis_ ? analysis_->lattice(old_def) : nullptr; }

    /// Returns lattice(@p old_def) if it differs from @p old_def (i.e. we learned something), otherwise `nullptr`.
    const Def* abstracted(const Def* old_def) const {
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

/// Rewrites the **current** World *in place* - unlike an RWPhase, which rebuilds it into a fresh one.
///
/// A mutable keeps its identity: only its ops() are Def::set anew, and only if the rewrite actually changed them.
/// So hash-consing makes every unaffected Def free instead of a per-run rebuild tax.
/// A mutable whose *type* changes is the one exception - identity is tied to the type - and falls back to an
/// RWPhase-style stub rebuild in this same World.
/// This matters most for the annex graph: it is proportional to the loaded plugins - not to the program - and a
/// *local* rewrite never touches it, yet an RWPhase re-creates all of it on **every** run.
///
/// Override skip() to prune subtrees that provably cannot change; this is what turns the traversal from
/// *"hash-cons every node"* into *"touch only what matters"*.
///
/// Since a change is only ever committed if it really is one, Phase::todo() is exact: a quiet run costs a pruned
/// traversal and nothing else.
///
/// @warning An InplaceRWPhase
/// * cannot immutabilize a mutable that the rewrite made vacuous (unless it takes the type-change fallback),
/// * must not hand out a fresh identity for something already in its target shape - that would never converge, and
/// * leaves what it replaced behind as garbage until the next Cleanup.
///
/// Use an RWPhase for anything else.
/// @see @ref phases_inplace_rwphase
class InplaceRWPhase : public Phase, public Rewriter {
public:
    /// @name Construction
    ///@{
    InplaceRWPhase(World& world, std::string name)
        : Phase(world, std::move(name))
        , Rewriter(world) {}
    InplaceRWPhase(World& world, flags_t annex)
        : Phase(world, annex)
        , Rewriter(world) {}
    ///@}

    /// @name Getters
    ///@{
    using Phase::world; ///< Disambiguates the Phase/Rewriter double base; for an InplaceRWPhase both are the same.
    ///@}

    /// @name Rewrite
    ///@{
    /// Runs the optional pre-analysis on world(), typically to a fixed point, before rewriting begins.
    /// If no analysis is needed, simply return `false`.
    virtual bool analyze() { return false; }

    /// Should start() walk the annex roots as well?
    /// An RWPhase *has* to: it must re-create every annex to populate the new World's table.
    /// An InplaceRWPhase finds that table already correct, so the annex graph - which is proportional to the loaded
    /// plugins, not to the program - is pure extra coverage here, and a *local* rewrite gains nothing from it:
    /// whatever the program actually uses is reached through the externals anyway.
    /// Hence this defaults to `false`; say `true` if your rewrite must also see *unused* annexes.
    virtual bool rewrite_annexes() const { return false; }

    virtual void rewrite_annex(flags_t, Sym, const Def*);
    virtual void rewrite_external(Def*);
    ///@}

protected:
    /// @name Rewrite
    ///@{
    /// Rewrites a *root* - i.e.\ an annex or an external.
    /// Defaults to rewrite(); override if roots need to be exempt from some of your rewrites.
    virtual const Def* rewrite_root(const Def* def) { return rewrite(def); }

    /// Cheap **O(1)** pruning hook: `true` iff @p def's subtree provably cannot change.
    /// Defaults to pruning nothing.
    virtual bool skip(const Def*) const { return false; }

    /// The skip() predicate for phases that only rewrite mutables and/or substitute Var%s:
    /// a *closed* immutable subtree contains neither.
    /// @warning Checking only local_muts() is **not** enough: a substitution installed via Rewriter::map would silently
    /// be dropped in a mut-free subtree that still mentions the substituted Var.
    static bool skip_closed_imm(const Def* def) {
        return !def->isa_mut() && def->local_muts().empty() && def->local_vars().empty();
    }

    void start() override;
    using Rewriter::rewrite; ///< Keeps the `Defs` overload visible next to the override below.
    /// Applies skip() and then delegates to rewrite_def(); override the latter, not this.
    const Def* rewrite(const Def* def) final { return skip(def) ? def : rewrite_def(def); }
    /// The actual rewrite hook - skip() has already said `false` for @p def.
    virtual const Def* rewrite_def(const Def* def) { return Rewriter::rewrite(def); }
    /// Keeps @p mut's identity and Def::set%s its ops anew iff rewriting them changed anything.
    const Def* rewrite_mut(Def* mut) override;
    ///@}
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
