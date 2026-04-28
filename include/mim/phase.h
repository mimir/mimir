#pragma once

#include <memory>

#include <fe/assert.h>
#include <fe/cast.h>

#include "mim/def.h"
#include "mim/nest.h"
#include "mim/pass.h"
#include "mim/rewrite.h"

namespace mim {

class Nest;
class Phase;
class PhaseMan;
class Repl;
class World;

using Repls  = std::deque<std::unique_ptr<Repl>>;
using Phases = std::deque<std::unique_ptr<Phase>>;

/// Unlike a Pass, a Phase performs one self-contained task and does not
/// interleave with other phases. Phases are intended to run in a classical sequence, one after another.
/// @see @ref phases_phase
class Phase : public Stage {
public:
    /// @name Construction & Destruction
    ///@{
    Phase(World& world, std::string name)
        : Stage(world, name) {}
    Phase(World& world, flags_t annex)
        : Stage(world, annex) {}

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
    ///@}

private:
    bool todo_ = false;

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

    /// Clears rewriter state, analysis state, and resets Phase::todo() for the next fixed-point iteration.
    /// @see RWPhase::analyze
    virtual void reset();
    ///@}

    /// @name Getters
    ///@{
    auto& lattice() { return lattice_; }
    const auto& lattice() const { return lattice_; }
    bool is_bootstrapping() const { return bootstrapping_; }
    ///@}

    /// @name Rewrite
    ///@{
    virtual void rewrite_annex(flags_t, const Def*);
    virtual void rewrite_external(Def*);
    Def* rewrite_mut(Def*) override; ///< Keeps old muts/vars.
    ///@}

    /// @name Getters
    ///@{
    World& world() { return Phase::world(); }
    ///@}

protected:
    void start() override;

    Def2Def lattice_;

private:
    bool bootstrapping_ = true;
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
    const Def* lattice(const Def* old_def) {
        if (auto i = analysis_->lattice().find(old_def); i != analysis_->lattice().end()) return i->second;
        return nullptr;
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
    virtual void rewrite_annex(flags_t, const Def*);
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

/// Simple Stage that searches for a pattern and replaces it.
/// Combine them in a ReplPhase.
class Repl : public Stage {
public:
    Repl(World& world, flags_t annex)
        : Stage(world, annex) {}

    virtual const Def* replace(const Def* def) = 0;
};

class ReplMan : public Repl {
public:
    ReplMan(World& world, flags_t annex)
        : Repl(world, annex) {}

    void apply(Repls&&);
    void apply(const App*) final;
    void apply(Stage& stage) final { apply(std::move(static_cast<ReplMan&>(stage).repls_)); }

    void add(std::unique_ptr<Repl>&& repl) { repls_.emplace_back(std::move(repl)); }
    const auto& repls() const { return repls_; }

private:
    const Def* replace(const Def*) final { fe::unreachable(); }

    Repls repls_;
};

#define MIM_CONCAT_INNER(a, b) a##b
#define MIM_CONCAT(a, b)       MIM_CONCAT_INNER(a, b)

#define MIM_REPL(__stages, __annex, ...) MIM_REPL_IMPL(__stages, __annex, __LINE__, __VA_ARGS__)

// clang-format off
#define MIM_REPL_IMPL(__stages, __annex, __id, ...)                         \
    struct MIM_CONCAT(Repl_, __id) : ::mim::Repl {                          \
        MIM_CONCAT(Repl_, __id)(::mim::World & world, ::mim::flags_t annex) \
            : Repl(world, annex) {}                                         \
                                                                            \
        const ::mim::Def* replace(const ::mim::Def* def) final __VA_ARGS__  \
    };                                                                      \
    ::mim::Stage::hook<__annex, MIM_CONCAT(Repl_, __id)>(__stages)
// clang-format on

class ReplManPhase : public RWPhase {
public:
    /// @name Construction
    ///@{
    ReplManPhase(World& world, std::unique_ptr<ReplMan>&& man)
        : RWPhase(world, "pass_man_phase")
        , man_(std::move(man)) {}
    ReplManPhase(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    void apply(const App*) final;
    void apply(Stage&) final;
    ///@}

    const ReplMan& man() const { return *man_; }

private:
    void start() final;
    const Def* rewrite(const Def*) final;

    std::unique_ptr<ReplMan> man_;
};

/// Removes unreachable and dead code by rebuilding the whole World into a new one and `swap`ping them afterwards.
/// @see @ref phases_rwphase
class Cleanup : public RWPhase {
public:
    Cleanup(World& world)
        : RWPhase(world, "cleanup") {}
    Cleanup(World& world, flags_t annex)
        : RWPhase(world, annex) {}
};

/// Wraps a PassMan pipeline as a Phase.
class PassManPhase : public Phase {
public:
    /// @name Construction
    ///@{
    PassManPhase(World& world, std::unique_ptr<PassMan>&& man)
        : Phase(world, "pass_man_phase")
        , man_(std::move(man)) {}
    PassManPhase(World& world, flags_t annex)
        : Phase(world, annex) {}

    void apply(const App*) final;
    void apply(Stage&) final;
    ///@}

    const PassMan& man() const { return *man_; }

private:
    void start() final { man_->run(); }

    std::unique_ptr<PassMan> man_;
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
    void apply(Stage&) final;
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
/// * If you are only interested in specific mutables, you can pass this to @p M.
/// @see @ref phases_closed_mut_phase
template<class M = Def>
class ClosedMutPhase : public Phase {
public:
    ClosedMutPhase(World& world, std::string name, bool elide_empty)
        : Phase(world, std::move(name))
        , elide_empty_(elide_empty) {}
    ClosedMutPhase(World& world, flags_t annex, bool elide_empty)
        : Phase(world, annex)
        , elide_empty_(elide_empty) {}

    bool elide_empty() const { return elide_empty_; }

protected:
    void start() override {
        world().template for_each<M>(elide_empty(), [this](M* mut) { root_ = mut, visit(mut); });
    }
    virtual void visit(M*) = 0;
    M* root() const { return root_; }

private:
    const bool elide_empty_;
    M* root_;
};

/// Like ClosedMutPhase but computes a Nest for each NestPhase::visit.
/// @see @ref phases_nest_phase
template<class M = Def>
class NestPhase : public ClosedMutPhase<M> {
public:
    NestPhase(World& world, std::string name, bool elide_empty)
        : ClosedMutPhase<M>(world, std::move(name), elide_empty) {}
    NestPhase(World& world, flags_t annex, bool elide_empty)
        : ClosedMutPhase<M>(world, annex, elide_empty) {}

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
