# Phases {#phases}

[TOC]

At a high level, a phase is an isolated compiler transformation or analysis step.
Phases are intended to do one thing at a time and to compose in a straightforward sequence.
See also the [Rewriting Guide](@ref rewriting), since several phase families are built directly on top of [`Rewriter`](@ref mim::Rewriter).

## Overview

A [`Phase`](@ref mim::Phase) has a single entry point, [`run()`](@ref mim::Phase::run), which wraps the actual implementation in [`start()`](@ref mim::Phase::start).

## Phase {#phases_phase}

[`Phase`](@ref mim::Phase) is the minimal base class.

A phase provides:

- a name or annex,
- access to the current [`World`](@ref mim::World),
- a [`run()`](@ref mim::Phase::run) wrapper for logging and verification,
- a [`todo()`](@ref mim::Phase::todo) accessor backed by the internal `todo_` flag for fixed-point iteration.

@note A phase requests another round by calling [`invalidate()`](@ref mim::Phase::invalidate).
- [`PhaseMan`](@ref mim::PhaseMan) uses this to drive fixed-point pipelines.
- [`RWPhase`](@ref mim::RWPhase) uses this to drive its optional pre-analysis to a fixed point.

### Typical Shape

A custom phase usually derives from [`Phase`](@ref mim::Phase) and implements [`start()`](@ref mim::Phase::start):

```cpp
class MyPhase : public mim::Phase {
public:
    MyPhase(World& world)
        : Phase(world, "my_phase") {}

private:
    void start() override {
        // do work here
    }
};
```

Run it with:

```cpp
mim::Phase::run<MyPhase>(world);
```

## Analysis {#phases_analysis}

[`Analysis`](@ref mim::Analysis) is the base class for phases that **inspect** the current world using the [`Rewriter`](@ref mim::Rewriter) traversal machinery.
It inherits from both [`Phase`](@ref mim::Phase) and [`Rewriter`](@ref mim::Rewriter), but unlike [`RWPhase`](@ref mim::RWPhase), it rewrites **into the same world**.
In practice, this means [`Rewriter`](@ref mim::Rewriter) is used as a structured, graph-aware traversal over ordinary MimIR [`Def`s](@ref mim::Def).

@note An [`Analysis`](@ref mim::Analysis) based on [`Rewriter`](@ref mim::Rewriter) has an abstract domain of ordinary [`Def`s](@ref mim::Def).

A central feature of [`Analysis`](@ref mim::Analysis) is its internal [`lattice()`](@ref mim::Analysis::lattice), which stores abstract information for old-world [`Def`s](@ref mim::Def) as a `Def2Def` mapping.

This is often convenient because analysis information can itself be represented as ordinary MimIR [`Def`s](@ref mim::Def).
As a result, existing IR machinery applies automatically, including:

- hash-consing / canonical sharing,
- built-in normalizations,
- and other simplifications already provided by the [`World`](@ref mim::World).

So if your abstract domain fits naturally into MimIR, you can often encode it directly as [`Def`s](@ref mim::Def) and store it in the analysis lattice.

An [`Analysis`](@ref mim::Analysis) visits:

1. all registered annex roots, then
2. all external mutables.

During the first part, [`mim::Analysis::is_bootstrapping()`](@ref mim::Analysis::is_bootstrapping) is `true`.
During the second part, it is `false`.

Typical usage:

- override [`rewrite()`](@ref mim::Rewriter::rewrite), [`rewrite_imm()`](@ref mim::Rewriter::rewrite_imm), [`rewrite_mut()`](@ref mim::Rewriter::rewrite_mut), or node-specific rewrite hooks,
- compute abstract information while traversing reachable IR,
- store that information in the lattice (see below) and/or in side tables,
- rely on [`lattice(concr, abstr)`](@ref mim::Analysis::lattice) to request another iteration automatically, or call [`invalidate()`](@ref mim::Phase::invalidate) manually where needed.

### Lattice API

The lattice follows these conventions:

- An *absent* entry means ⊥ - nothing is known yet.
- An entry mapping a definition **to itself** (`def ↦ def`) means ⊤ - "no useful information, keep as-is".
- Anything else is a discovered abstract value; analyses may also introduce their own sentinels in between as ordinary `Def`s (e.g. SEO's GVN-bundle and pending-⊤ proxies).

[`Analysis`](@ref mim::Analysis) provides the following accessors and mutators:

- [`lattice()`](@ref mim::Analysis::lattice) returns the full lattice map.
- [`lattice(def)`](@ref mim::Analysis::lattice) returns the recorded abstract value for `def`, or `nullptr` if nothing is known.
- [`lattice(concr, abstr)`](@ref mim::Analysis::lattice) writes `concr ↦ abstr` into both the lattice and the rewriter map (so future rewrites of `concr` short-circuit to `abstr`) and **automatically** [`invalidate`s](@ref mim::Phase::invalidate) iff this changes observable information: an existing entry was overwritten, or a fresh fact other than ⊤ was inserted.
  Freshly inserting ⊤ (`def ↦ def`) stays silent, as it is indistinguishable from an *absent* entry for consumers.
  It returns `true` iff it changed observable information - i.e. iff it invalidated - so the caller can still react, e.g. log.
  Besides recording a lattice fact, it also seed the rewriter map, so a later [`rewrite()`](@ref mim::Rewriter::rewrite) of `concr` immediately returns `abstr`.
  It `assert`s, if you go down from ⊤.
- [`lattice_force(concr, abstr)`](@ref mim::Analysis::lattice_force) is the non-`assert`ing variant.
- [`pin(def)`](@ref mim::Analysis::pin) monotonically forces `def` to ⊤.
  Being built on [`lattice(concr, abstr)`](@ref mim::Analysis::lattice), it invalidates iff it overwrote previous information.
- [`is_top(def)`](@ref mim::Analysis::is_top) checks for `def ↦ def`.

Analysis-specific sentinels should be ordinary `Def`s - e.g. a dedicated [`Proxy`](@ref mim::Proxy) tag, as SEO uses for its GVN and pending-⊤ markers - never `nullptr`, which is reserved for *absent*.

### Handling of Mutables

Unlike [`RWPhase`](@ref mim::RWPhase), an [`Analysis`](@ref mim::Analysis) must traverse the entire reachable program without rebuilding it.
For this reason, [`Analysis`](@ref mim::Analysis) overrides [`rewrite_mut()`](@ref mim::Analysis::rewrite_mut) to keep mutables in place and use the rewriter machinery as a graph-aware traversal over the existing world.

Immutables are still visited **depth-first** through the inherited [`Rewriter`](@ref mim::Rewriter) recursion, but mutables are visited **breadth-first** via an internal worklist.
This matters because abstract values typically flow *between* mutables — e.g. from an `App` call site into the callee's binder vars.
A breadth-first order tends to seed a mutable from **all** of its predecessors *before* its body is walked, so information propagates further per fixed-point round and convergence needs fewer rounds.
Breadth-first traversal is safe here precisely because an [`Analysis`](@ref mim::Analysis) never rebuilds a mutable: it maps every mutable to itself, so nothing depends on a mutable being fully rewritten before it is used (contrast the strict depth-first ordering an [`RWPhase`](@ref mim::RWPhase) needs, where a rebuilt binder's type/identity is consumed as it is constructed).

[`rewrite_mut()`](@ref mim::Analysis::rewrite_mut):

1. returns immediately if the mutable was already scheduled this round (see below),
2. records the mutable as visited via `mut -> mut`, and
3. **enqueues** it on the worklist — it does *not* recurse into the body itself.

Once a batch of roots has been scheduled, [`Analysis::drain()`](@ref mim::Analysis) pops mutables from the worklist and, for each, enters it for [`curr_mut()`](@ref mim::Rewriter::curr_mut) tracking and rewrites its [dependencies](@ref mim::Def::deps).
Rewriting those dependencies schedules any further mutables it reaches, so the worklist drains in breadth-first order.

The `mut -> mut` entry recorded in step 2 doubles as the per-round *"already scheduled"* marker: it lives in the rewriter map (see [`lookup()`](@ref mim::Rewriter::lookup)), which [`reset()`](@ref mim::Analysis::reset) clears at the start of every round.
Hence each mutable's dependencies are walked **at most once per fixed-point round**, which also prevents cyclic (recursive) CFGs from recursing forever.

@warning Because [`rewrite_mut()`](@ref mim::Analysis::rewrite_mut) enqueues instead of dispatching by node, the node-specific `rewrite_mut_*` hooks (e.g. `rewrite_mut_Lam`) are **never invoked** for an [`Analysis`](@ref mim::Analysis).
Override [`rewrite_mut()`](@ref mim::Analysis::rewrite_mut) itself (or the `rewrite_imm_*` hooks, which dispatch as usual) instead.

When a `rewrite_imm_App` override propagates abstract values from call arguments into a callee's binder vars, it should seed those lattice entries first and then simply [`rewrite()`](@ref mim::Rewriter::rewrite) the callee: this schedules the callee (or is a no-op if already scheduled) so its body is walked later during the drain, by which point the seeded facts — and any joins contributed by sibling call sites — are in place.
[`lattice(concr, abstr)`](@ref mim::Analysis::lattice) conveniently pairs the two writes (lattice and rewriter map) that arise in this seeding pattern.

### Sparse Fixed-Point Iteration

A **full** round traverses the whole [`World`](@ref mim::World): [`start()`](@ref mim::Analysis) first runs [`prepare()`](@ref mim::Analysis::prepare), then rewrites all annex roots, drains the worklist, does the same for the external mutables, and finally runs [`finalize()`](@ref mim::Analysis::finalize).
Whenever [`lattice(concr, abstr)`](@ref mim::Analysis::lattice) changes an entry it [`invalidate`s](@ref mim::Phase::invalidate), requesting another round, and records [`curr_mut()`](@ref mim::Rewriter::curr_mut) as *dirty*.

Only the first round (and certification rounds, see below) is full; a follow-up round is **sparse**: it re-drains only the dirty mutables — plus everything reachable from them — instead of walking the whole World.
At the start of a sparse round the accumulated lattice is replayed into the rewriter map, so a dirty mutable's body sees the substitutions its (non-revisited) producers installed in earlier rounds.
An analysis can [`taint()`](@ref mim::Analysis::taint) additional mutables when a change must re-visit more than the writer — e.g. SEO taints all call sites of a `Lam` whose abstract vars changed, which keeps its per-round join restart sound.
A change that cannot be attributed to any mutable (during the annex walk or [`finalize()`](@ref mim::Analysis::finalize)) forces the next round to be full.

Since dirt tracks *writers* — not readers — a sparse round may miss affected mutables.
Hence, once sparse rounds quiesce, one final **full** round certifies the fixed point; if it discovers new facts, iteration continues sparsely from its dirt.
Only full rounds run [`finalize()`](@ref mim::Analysis::finalize), so post-passes always see the complete abstract World.
Use [`make_dense()`](@ref mim::Analysis::make_dense) to force whole-World rounds unconditionally.

### Reset Between Iterations

If an analysis participates in a fixed-point loop, it should be ready to run multiple times.
The base [`reset()`](@ref mim::Analysis::reset) clears the rewriter map (and hence the per-round *"already scheduled"* markers) and the worklist, and resets [`Phase::todo()`](@ref mim::Phase::todo) for the next round, but **preserves** [`lattice()`](@ref mim::Analysis::lattice) so that abstract values accumulated in earlier iterations remain available — this is what makes fixed-point convergence possible.

## RWPhase {#phases_rwphase}

[`RWPhase`](@ref mim::RWPhase) is the base class for phases that **rebuild the current world into a new one**, thereby eliminating garbage.
This is the standard base class for optimization phases that structurally transform IR.

It inherits from both [`Phase`](@ref mim::Phase) and [`Rewriter`](@ref mim::Rewriter), but here the two worlds differ:

- [`Phase::world`](@ref mim::Phase::world) is the **old** world,
- [`Rewriter::world`](@ref mim::Rewriter::world) is the **new** world.

@note To avoid confusion, direct `world()` access is deleted.
Use:
- [`old_world()`](@ref mim::RWPhase::old_world) to inspect existing IR,
- [`new_world()`](@ref mim::RWPhase::new_world) to build rewritten IR.

### Cleanup

[`Cleanup`](@ref mim::Cleanup) is simply an [`RWPhase`](@ref mim::RWPhase) with no custom rewrites.
Because an [`RWPhase`](@ref mim::RWPhase) reconstructs only what is reachable from the world roots, rebuilding automatically eliminates dead and unreachable code.

### Execution Model

An [`RWPhase`](@ref mim::RWPhase) runs in three conceptual steps:

1. optionally perform a fixed-point analysis **on the old world**,
2. rewrite reachable old [`Def`s](@ref mim::Def) **into the new world**:
   1. rewrite annex roots,
   2. rewrite external mutables;
3. swap the **old** and **new** worlds.

After the swap, the rewritten world becomes the current one.

### Optional Pre-Analysis

An [`RWPhase`](@ref mim::RWPhase) may be given an associated [`Analysis`](@ref mim::Analysis).
If so, [`analyze()`](@ref mim::RWPhase::analyze) runs that analysis to a fixed point before rewriting begins.

This is a common pattern:

- the analysis computes facts on the old world,
- those facts are stored in [`Analysis::lattice()`](@ref mim::Analysis::lattice) and/or auxiliary side tables,
- the rewrite queries them through [`RWPhase::lattice()`](@ref mim::RWPhase::lattice) and produces the new world.

If no analysis is needed, [`analyze()`](@ref mim::RWPhase::analyze) can simply return `false`.

### Analysis Results

Once [`analyze()`](@ref mim::RWPhase::analyze) has run, the rewrite can query the analysis result through [`RWPhase::lattice()`](@ref mim::RWPhase::lattice).

This provides read access to the analysis lattice **for old-world [`Def`s](@ref mim::Def)**:
given an old definition, [`RWPhase::lattice()`](@ref mim::RWPhase::lattice) returns the abstract value computed by the associated [`Analysis`](@ref mim::Analysis), or `nullptr` if no value is available.

This is the standard way to communicate fixed-point analysis results into the subsequent rewrite.

### Bootstrapping

Like [`Analysis`](@ref mim::Analysis), [`RWPhase`](@ref mim::RWPhase) processes annex roots before externals.

While annexes are being rewritten, [`mim::RWPhase::is_bootstrapping()`](@ref mim::RWPhase::is_bootstrapping) is `true`.

This matters because annexes may depend on one another.
During bootstrapping, rewrites that refer to other annexes may need to be deferred or skipped, since those annexes might not yet exist in the new world.

### Typical Shape

```cpp
class MyRWPhase : public mim::RWPhase {
public:
    MyRWPhase(World& world)
        : RWPhase(world, "my_rw_phase") {}

private:
    const Def* rewrite_imm_App(const App* app) override {
        // customize rebuilding here
        return RWPhase::rewrite_imm_App(app);
    }
};
```

Run it with:

```cpp
mim::Phase::run<MyRWPhase>(world);
```

## InplaceRWPhase {#phases_inplace_rwphase}

[`InplaceRWPhase`](@ref mim::InplaceRWPhase) rewrites the **current** world *in place* instead of rebuilding it into a fresh one.

A mutable keeps its identity: only its [`ops()`](@ref mim::Def::ops) are [`Def::set`](@ref mim::Def::set) anew, and only if the rewrite actually changed them.
So hash-consing makes every unaffected [`Def`](@ref mim::Def) free instead of a per-run rebuild tax.
A mutable whose *type* changes is the one exception — identity is tied to the type — and falls back to an [`RWPhase`](@ref mim::RWPhase)-style stub rebuild in this same world.
This matters most for the annex graph: it is proportional to the loaded plugins — not to the program — and a *local* rewrite never touches it, yet an [`RWPhase`](@ref mim::RWPhase) re-creates all of it on **every** run.

It inherits from both [`Phase`](@ref mim::Phase) and [`Rewriter`](@ref mim::Rewriter), but unlike an [`RWPhase`](@ref mim::RWPhase) both refer to the *same* world, so plain `world()` is what you want.

### Restrictions

An [`InplaceRWPhase`](@ref mim::InplaceRWPhase)

- cannot immutabilize a mutable that the rewrite made vacuous (unless it takes the type-change fallback),
- must not hand out a fresh identity for something that already is in its target shape — since [`Phase::todo()`](@ref mim::Phase::todo) is exact, that would never converge, and
- leaves what it replaced behind as garbage until the next [`Cleanup`](@ref mim::Cleanup).

Use an [`RWPhase`](@ref mim::RWPhase) for anything else.

### Pruning

Override [`skip()`](@ref mim::InplaceRWPhase::skip) with a cheap **O(1)** predicate that is `true` iff a [`Def`](@ref mim::Def)'s subtree provably cannot change.
This is what turns the traversal from *"hash-cons every node"* into *"touch only what matters"*.

[`skip_closed_imm()`](@ref mim::InplaceRWPhase::skip_closed_imm) is the ready-made predicate for phases that only rewrite mutables and/or substitute [`Var`s](@ref mim::Var): an immutable with neither [`local_muts()`](@ref mim::Def::local_muts) nor [`local_vars()`](@ref mim::Def::local_vars) contains neither.
Both [`BetaRed`](@ref mim::BetaRed) and [`EtaConv`](@ref mim::EtaConv) use it.

@warning Testing only [`local_muts()`](@ref mim::Def::local_muts) is **not** enough: a substitution installed via [`Rewriter::map()`](@ref mim::Rewriter::map) would silently be dropped in a mut-free subtree that still mentions the substituted [`Var`](@ref mim::Var).

### Roots

By default, an [`InplaceRWPhase`](@ref mim::InplaceRWPhase) walks only the **external** roots.
[`rewrite_annexes()`](@ref mim::InplaceRWPhase::rewrite_annexes) returns `false`, because an [`RWPhase`](@ref mim::RWPhase) only *has* to walk the annexes in order to populate the new world's annex table — a phase that stays in the same world finds that table already correct.
Whatever the program actually uses is reached through the externals anyway.
Say `true` if your rewrite must also see *unused* annexes.

[`rewrite_root()`](@ref mim::InplaceRWPhase::rewrite_root) is the hook for rewrites that must exempt roots; [`EtaConv`](@ref mim::EtaConv) uses it so that an annex or external keeps its η-shape.

### Fixed Points

Since a change is only ever committed if it really is one, [`Phase::todo()`](@ref mim::Phase::todo) is exact: a quiet run costs a pruned traversal and nothing else.
This is what makes an [`InplaceRWPhase`](@ref mim::InplaceRWPhase) cheap to re-run inside a [`PhaseMan`](@ref mim::PhaseMan) fixed-point loop.

### Typical Shape

```cpp
class MyInplacePhase : public mim::InplaceRWPhase {
public:
    MyInplacePhase(World& world)
        : InplaceRWPhase(world, "my_inplace_phase") {}

private:
    bool skip(const Def* def) const final { return skip_mutless(def); }

    const Def* rewrite_imm_App(const App* app) final {
        // customize rewriting here
        return Rewriter::rewrite_imm_App(app);
    }
};
```

## PhaseMan {#phases_phase_man}

[`PhaseMan`](@ref mim::PhaseMan) organizes several phases into a pipeline.

It can run them:

- once, in sequence, or
- repeatedly to a fixed point.

A fixed-point [`PhaseMan`](@ref mim::PhaseMan) reruns the pipeline as long as at least one phase [`invalidate`s](@ref mim::Phase::invalidate).
Since a phase's run is a deterministic function of the [`World`](@ref mim::World)'s content, [`PhaseMan`](@ref mim::PhaseMan) **skips** any phase whose last run was quiet and after which no other phase changed the world - so tail iterations only rerun the phases that are still making progress, and the pipeline terminates without a final everybody-reruns round.

Before a rerun, the phase is recreated from its original configuration.
This keeps phase-local state from leaking across rounds unless the phase explicitly recomputes it.

@note [`PhaseMan`](@ref mim::PhaseMan) is the orchestration layer for classical phase pipelines.

### Typical Shape

```cpp
auto phases = mim::Phases();
phases.emplace_back(std::make_unique<PhaseA>(world));
phases.emplace_back(std::make_unique<PhaseB>(world));

mim::PhaseMan man(world, mim::Annex::base<mim::plug::compile::phases>());
man.apply(/*fixed_point=*/true, std::move(phases));
man.run();
```

Use a fixed-point pipeline when phases expose new optimization opportunities for one another.

## ClosedMutPhase {#phases_closed_mut_phase}

[`ClosedMutPhase`](@ref mim::ClosedMutPhase) is a traversal helper for phases that visit all reachable, **closed** mutables in the world.

A mutable is relevant here if it is:

- reachable,
- closed, i.e. it has no free variables,
- optionally non-empty, depending on `elide_empty`.

This is useful for local analyses or transformations naturally phrased as:

@note For every reachable closed mutable, inspect or process it.

You override `visit(M*)`, where `M` defaults to [`Def`](@ref mim::Def) but may be restricted to a particular mutable subtype.

### Typical Shape

```cpp
class MyClosedPhase : public mim::ClosedMutPhase<Lam> {
public:
    MyClosedPhase(World& world)
        : ClosedMutPhase(world, "my_closed_phase", /*elide_empty=*/true) {}

private:
    void visit(Lam* lam) override {
        // process each reachable closed Lam
    }
};
```

### NestPhase {#phases_nest_phase}

[`NestPhase`](@ref mim::NestPhase) builds on [`ClosedMutPhase`](@ref mim::ClosedMutPhase) and computes a [`Nest`](@ref mim::Nest) for each visited mutable.

Use it when your phase needs a structured view of nested control or binding structure rather than just the raw mutable.

Instead of overriding `visit(M*)`, override:

```cpp
visit(const Nest&)
```

This is convenient for analyses that reason about nesting, dominance-like structure, or hierarchical regions.

## Example: SCCP

\include "examples/sccp.h"

The provided [Sparse Conditional Constant Propagation (SCCP)](https://en.wikipedia.org/wiki/Sparse_conditional_constant_propagation) implementation is a good example of the intended phase structure.
Its architecture is:

- an inner [`Analysis`](@ref mim::Analysis) computes propagation facts on the old world,
- an outer [`RWPhase`](@ref mim::RWPhase) uses those facts to rebuild a simplified new world.

@note The implementation propagates not only constants but also arbitrary expressions.

### Analysis

\include "examples/sccp_analysis.cpp"

The SCCP analysis associates each lambda variable with a lattice value:

- bottom: no useful information yet (an *absent* entry),
- a concrete expression: this value can be propagated,
- top: keep the variable as-is (a `Def` maps to itself).

In the implementation, this lattice is stored in [`Analysis::lattice()`](@ref mim::Analysis::lattice) as a `Def2Def` map.
A nice aspect here is that the propagated value is itself a regular [`Def`](@ref mim::Def).
This illustrates the benefit of building analysis on top of [`Rewriter`](@ref mim::Rewriter): the abstract domain can live directly inside MimIR, so canonicalization and normalization come for free.

The join in `propagate()` is expressed entirely through the lattice API:
[`lattice(var)`](@ref mim::Analysis::lattice) reads the current abstract value,
[`lattice(concr, abstr)`](@ref mim::Analysis::lattice) overwrites it, and
[`pin()`](@ref mim::Analysis::pin) resolves conflicting values to ⊤.
No manual [`invalidate()`](@ref mim::Phase::invalidate) bookkeeping is needed: every join step that gains information - including the ⊥ → value insert - triggers the next fixed-point round automatically via [`lattice(concr, abstr)`](@ref mim::Analysis::lattice).

The analysis traverses the old world and updates the lattice when it sees applications of optimizable lambdas.
Whenever this changes the lattice, the analysis reruns until stable - sparsely, re-draining only the dirty mutables in between full rounds.
This is a textbook use of [`Analysis`](@ref mim::Analysis):

- walk the old IR,
- collect facts,
- store them in [`Analysis::lattice()`](@ref mim::Analysis::lattice),
- iterate to a fixed point.

### SSA without Dominance {#ssa-without-dominance}

The very first line of `propagate()` is a guard that has no counterpart in the lattice algebra:

```cpp
if (lam_of(var)->nests(def)) return pin(var);
```

It is the MimIR analogue of the *dominance* side condition that a classical SSA-based SCCP has to enforce, so it is worth spelling out what it replaces.

#### Why the guard exists at all

Textbook SCCP only ever propagates **constants**.
A constant is a literal: it has no operands and is available at *every* program point by construction.
This is what makes classical SCCP so comfortable — specializing a call site to a constant is *always* valid, and there is simply no availability question to ask.

MimIR's SCCP is more ambitious: it propagates **arbitrary expressions**, not just constants (this is essentially copy/expression propagation folded into the same fixed point).
The moment you propagate a whole expression, you inherit an obligation constants let you ignore: the expression you substitute must actually be *available* at the point where it lands.
The guard is exactly that availability check.

#### The classical picture

Textbook SCCP runs on a CFG in SSA form.
Every value has exactly one definition, control flow is made explicit by basic blocks and edges, and φ-nodes reconcile the values that arrive along the different predecessor edges of a join block.
SCCP assigns each SSA value a lattice cell (⊥ / a constant / ⊤) and, once the fixed point is reached, substitutes the discovered constant at *every* use of that value.

That substitution is only sound because SSA comes with a **dominator tree**:

- a definition dominates all of its uses, and
- a φ-operand must be available along its associated predecessor edge, i.e. its definition dominates the end of that predecessor block.

Dominance is exactly the structural guarantee *"the value already exists at the program point where I want to use it"*.
Without it, folding a value into a use could move a computation to a place where its operands are not yet defined.

#### The MimIR picture

MimIR has no CFG, no basic blocks, and no separate φ instructions.
Control flow is expressed in CPS: a [`Lam`](@ref mim::Lam) *is* a basic block, its parameters *are* the φ-nodes, and every [`App`](@ref mim::App) of that `Lam` is one *predecessor edge* supplying the corresponding operands.
So the SCCP analysis joins, per parameter, all the arguments flowing in from the call sites — precisely the φ-semantics — and stores the result in the [`lattice()`](@ref mim::Analysis::lattice).

What is missing is the dominator tree.
Its role — deciding whether a candidate value is *available* at the point where it would be substituted — is taken over by the scope/nesting relation [`Def::nests`](@ref mim::Def::nests), computed structurally from free variables rather than from a precomputed CFG analysis.
`L->nests(def)` holds iff `def` lives *strictly inside* `L`, i.e. it transitively depends on binders introduced below `L`; a `def` that only mentions things visible at `L`'s level or further out is **not** nested.

Now the guard reads directly:

- `var` is a parameter of `L = lam_of(var)`; its call sites live *outside* `L`.
- If `L->nests(def)`, the joined value refers to binders that only come into existence *within* `L`'s own body.
  Such a value simply does not exist at `L`'s call sites, so propagating it into `var` — and thus substituting it at `var`'s uses — would hoist a computation out of the region where its operands are defined.
  This is the exact situation dominance forbids, so the analysis pins `var` to ⊤ ([`pin`](@ref mim::Analysis::pin)) instead.
- If `L` does *not* nest `def`, the value is in scope at every call site — the analogue of *"the definition dominates all uses"* — and propagation is sound.

In other words, where classical SCCP walks a dominator tree to certify availability, MimIR asks a single scope question: *is this value visible at the binder it would replace?*
[`Def::nests`](@ref mim::Def::nests) is that availability oracle, and it falls straight out of the free-variable structure that MimIR maintains anyway — no auxiliary dominance computation required.

#### Why this is hard elsewhere

The availability obligation is cheap to *state* but awkward to *discharge* in most IRs, and this is where MimIR's structural answer stands out.

- **CFG + SSA** answers it with the dominator tree, as sketched above.
  This works, but only because the CFG has already fixed *where* every value lives; the whole machinery presupposes a schedule.
- **Sea-of-nodes** deliberately refuses that commitment: data nodes float, and only control, φ, and memory nodes are pinned.
  That freedom is the entire point — it is what lets the optimizer move computations around without fighting a premature schedule.
  But it makes availability ill-posed: a floating expression has no location, so *"is it available here?"* is not even a well-formed question until the node is anchored.
  To answer it you must reason about where the expression's transitively control-pinned inputs would sit — that is, run (at least partial) global code motion and consult the CFG dominator relation.
  So copy/expression propagation drags the schedule — precisely what sea-of-nodes set out to avoid — back into the picture.

MimIR sidesteps the dilemma without ever introducing a syntactic scope: it is a *scopeless* IR.
There are no lexical scoping brackets that a `Lam` opens over its body; instead, scope is *implicit*, emerging from how free variables nest.
[`Def::nests`](@ref mim::Def::nests) reads availability straight off that implicit nesting — `L` nests `def` iff `def` transitively depends on a variable bound below `L` — so the containment a lexical language would spell out with explicit brackets is recovered purely from the free-variable structure MimIR maintains anyway.
The query is structural and commits to no schedule, so MimIR gets a compelling, schedule-free answer to the availability question that dominance-based and sea-of-nodes IRs can only reconstruct by (partially) scheduling first.

### Transformation

\include "examples/sccp_transform.cpp"

Once the lattice is stable, the outer SCCP phase starts rewriting.
During rewriting, it can query abstract values for old-world definitions through [`RWPhase::lattice()`](@ref mim::RWPhase::lattice).

When it sees an application of a lambda whose parameters have propagated values, it rebuilds a specialized lambda:

- parameters with known propagated expressions are removed,
- remaining parameters are kept,
- the lambda body is rewritten with the propagated values substituted,
- the call site is rebuilt with only the remaining arguments.

So SCCP follows the standard [`RWPhase`](@ref mim::RWPhase) pattern:

1. analyze the old world,
2. rewrite into a new world using the computed facts,
3. swap worlds.

### Discussion

Separating SCCP into analysis and rewrite keeps both parts simple:

- the analysis never mutates or partially rewrites the program,
- the rewrite does not need to discover facts on the fly,
- fixed-point logic stays in the analysis stage where it belongs,
- the handoff from analysis to rewrite is explicit through [`Analysis::lattice()`](@ref mim::Analysis::lattice) and [`RWPhase::lattice()`](@ref mim::RWPhase::lattice).

This separation is the main design pattern to follow for nontrivial optimizations.

@note
The complete SCCP example fits into roughly 150 lines of C++ source code.
Most of the usual compiler boilerplate is absorbed by the existing [`Analysis`](@ref mim::Analysis), [`RWPhase`](@ref mim::RWPhase), and [`Rewriter`](@ref mim::Rewriter) infrastructure, so the implementation can focus on the optimization itself.

## Choosing the Right Base Class

A useful rule of thumb is:

- derive from [`Phase`](@ref mim::Phase) if you just need a custom one-off action,
- derive from [`Analysis`](@ref mim::Analysis) if you want a graph-aware traversal that computes facts on the current world,
- derive from [`RWPhase`](@ref mim::RWPhase) if you want to rebuild the world into a transformed new one, optionally consuming facts from an associated [`Analysis`](@ref mim::Analysis),
- derive from [`InplaceRWPhase`](@ref mim::InplaceRWPhase) if your rewrite is type-preserving and *local*, so that paying for a full rebuild would be wasteful,
- derive from [`ClosedMutPhase`](@ref mim::ClosedMutPhase) if you want to visit all reachable closed mutables,
- derive from [`NestPhase`](@ref mim::NestPhase) if that visit should come with a computed [`Nest`](@ref mim::Nest).

## Recommended Design Pattern

For most optimization phases, the preferred structure is:

1. write an [`Analysis`](@ref mim::Analysis) that computes facts to a fixed point,
2. store those facts in [`Analysis::lattice()`](@ref mim::Analysis::lattice) and/or auxiliary tables,
3. write an [`RWPhase`](@ref mim::RWPhase) that consumes those facts while rebuilding the world.

This keeps analyses and transformations cleanly separated and fits naturally with MimIR’s rewriting-based infrastructure.

## Minimal Examples

A simple whole-world rewrite phase:

```cpp
class Simplify : public mim::RWPhase {
public:
    Simplify(mim::World& world)
        : RWPhase(world, "simplify") {}

private:
    const mim::Def* rewrite_imm_App(const mim::App* app) override {
        // rewrite or simplify selected applications
        // fallback:
        return Rewriter::rewrite_imm_App(app);
    }
};
```

A simple analysis phase:

```cpp
class CountMutLams : public mim::Analysis {
public:
    CountMutLams(mim::World& world)
        : Analysis(world, "count_lams") {}

    size_t num_lams = 0;

private:
    // Note: override rewrite_mut - the node-specific rewrite_mut_* hooks are not dispatched
    // for an Analysis (see "Handling of Mutables").
    mim::Def* rewrite_mut(mim::Def* mut) override {
        if (!lookup(mut) && mut->isa_mut<mim::Lam>()) ++num_lams; // count on first visit only
        return mim::Analysis::rewrite_mut(mut);
    }
};
```

Using both:

```cpp
CountMutLams analysis(world);
analysis.run();

mim::Phase::run<Simplify>(world);
```

<!-- Keep the invisible separator in `M⁠im` so Doxygen does not link this heading to the `mim` namespace in the TOC. -->
### Compilation Pipelines in M⁠im

You can also expose your custom phases as axioms in Mim via the [compile plugin](@ref compile) and build your own compilation pipeline.
Mim's default compilation pipeline is defined in the [opt plugin](@ref opt).

## Summary

Phases are MimIR’s main unit of compiler work.

- [`Phase`](@ref mim::Phase) is the minimal base abstraction.
- [`Analysis`](@ref mim::Analysis) is for graph-aware fact collection on the current world and provides a reusable [`lattice()`](@ref mim::Analysis::lattice) for abstract values.
- [`RWPhase`](@ref mim::RWPhase) is for rewriting the current world into a transformed new one and can read analysis results through [`RWPhase::lattice()`](@ref mim::RWPhase::lattice).
- [`InplaceRWPhase`](@ref mim::InplaceRWPhase) is for type-preserving, local rewrites of the current world that must not pay for a rebuild.
- [`PhaseMan`](@ref mim::PhaseMan) sequences phases, optionally to a fixed point.
- [`ClosedMutPhase`](@ref mim::ClosedMutPhase) and [`NestPhase`](@ref mim::NestPhase) are traversal helpers for common whole-world inspections.

The key design idea is that MimIR phases are built around structured traversal and rewriting.
For substantial optimizations, the usual pattern is:

- compute facts with [`Analysis`](@ref mim::Analysis),
- store them in [`Analysis::lattice()`](@ref mim::Analysis::lattice),
- consume them with [`RWPhase`](@ref mim::RWPhase).
