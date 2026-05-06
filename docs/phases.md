# Phases {#phases}

[TOC]

At a high level, a phase is an isolated compiler transformation or analysis step.
Phases are intended to do one thing at a time and to compose in a straightforward sequence.
See also the [Rewriting Guide](@ref rewriting), since several phase families are built directly on top of [`Rewriter`](@ref mim::Rewriter).

## Overview

A [`Phase`](@ref mim::Phase) is a [`Stage`](@ref mim::Stage) with a single entry point, [`run()`](@ref mim::Phase::run), which wraps the actual implementation in [`start()`](@ref mim::Phase::start).

## Phase {#phases_phase}

[`Phase`](@ref mim::Phase) is the minimal base class.

A phase has:

- a name or annex,
- access to the current [`World`](@ref mim::World),
- a [`run()`](@ref mim::Phase::run) wrapper for logging and verification,
- a [`todo()`](@ref mim::Phase::todo) accessor backed by the internal `todo_` flag for fixed-point iteration.

@note
A phase sets this flag via [`invalidate()`](@ref mim::Phase::invalidate) when it discovers that another round is required.
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
- store that information in [`lattice()`](@ref mim::Analysis::lattice) and/or in side tables,
- call [`invalidate()`](@ref mim::Phase::invalidate) if new information was discovered and another iteration is required.

### Handling of Mutables

Unlike [`RWPhase`](@ref mim::RWPhase), an [`Analysis`](@ref mim::Analysis) must traverse the entire reachable program without rebuilding it.
For this reason, [`Analysis`](@ref mim::Analysis) overrides [`rewrite_mut()`](@ref mim::Analysis::rewrite_mut) to *preserve mutables and their binders* instead of rewriting them.

When a mutable is visited, the analysis immediately maps it to itself (`mut -> mut`), and likewise maps its associated variable(s) to themselves (`var -> var`).
This keeps the mutable structure intact as a stable "skeleton program" and prevents the analysis from accidentally introducing new mutables or fresh variables.

After establishing these identity mappings, the analysis recursively traverses the mutable’s [dependencies](@ref mim::Def::deps) via [`rewrite()`](@ref mim::Rewriter::rewrite).
This uses the [`Rewriter`](@ref mim::Rewriter) machinery purely as a structured, graph-aware traversal, while all computed information is stored separately in [`Analysis::lattice()`](@ref mim::Analysis::lattice).

A common convention is to encode **top** as `def -> def` in the lattice:
mapping a definition to itself means "no useful information, keep as-is", while mapping it to a different [`Def`](@ref mim::Def) represents a discovered abstract value.

### Reset Between Iterations

If an analysis participates in a fixed-point loop, it should be ready to run multiple times.
The base [`reset()`](@ref mim::Analysis::reset) clears the rewriter state and resets the internal fixed-point state for the next round.

## RWPhase {#phases_rwphase}

[`RWPhase`](@ref mim::RWPhase) is the base class for phases that **rebuild the current world into a new one**, thereby eliminating garbage.
This is the standard base class for optimization phases that structurally transform IR.

It inherits from both [`Phase`](@ref mim::Phase) and [`Rewriter`](@ref mim::Rewriter), but here the two worlds differ:

- [`Phase::world`](@ref mim::Stage::world) is the **old** world,
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

An [`RWPhase`](@ref mim::RWPhase) may be associated with an [`Analysis`](@ref mim::Analysis).
If so, the rewrite can query the analysis result through [`RWPhase::lattice()`](@ref mim::RWPhase::lattice).

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
        return Rewriter::rewrite_imm_App(app);
    }
};
```

Run it with:

```cpp
mim::Phase::run<MyRWPhase>(world);
```

## PhaseMan {#phases_phase_man}

[`PhaseMan`](@ref mim::PhaseMan) organizes several phases into a pipeline.

It can run them:

- once, in sequence, or
- repeatedly to a fixed point.

A fixed-point [`PhaseMan`](@ref mim::PhaseMan) reruns the whole pipeline as long as at least one phase [`invalidate`s](@ref mim::Phase::invalidate).

Between iterations, each phase is recreated from its original configuration.
This keeps phase-local state from leaking across rounds unless the phase explicitly recomputes it.

@note [`PhaseMan`](@ref mim::PhaseMan) is the orchestration layer for classical phase pipelines.

### Typical Shape

```cpp
auto phases = mim::Phases();
phases.emplace_back(std::make_unique<PhaseA>(world));
phases.emplace_back(std::make_unique<PhaseB>(world));

mim::PhaseMan man(world, mim::plug::compile::phases);
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

- bottom: no useful information yet,
- a concrete expression: this value can be propagated,
- top: keep the variable as-is (a `Def` maps to itself).

In the implementation, this lattice is stored in [`Analysis::lattice()`](@ref mim::Analysis::lattice) as a `Def2Def` map.
A nice aspect here is that the propagated value is itself a regular [`Def`](@ref mim::Def).
This illustrates the benefit of building analysis on top of [`Rewriter`](@ref mim::Rewriter): the abstract domain can live directly inside MimIR, so canonicalization and normalization come for free.

The analysis traverses the old world and updates the lattice when it sees applications of optimizable lambdas.
If new information is discovered, it [`invalidate`s](@ref mim::Phase::invalidate), causing the analysis to rerun until stable.
This is a textbook use of [`Analysis`](@ref mim::Analysis):

- walk the old IR,
- collect facts,
- store them in [`Analysis::lattice()`](@ref mim::Analysis::lattice),
- iterate to a fixed point.

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
    const mim::Def* rewrite_mut_Lam(mim::Lam* lam) override {
        ++num_lams;
        return mim::Analysis::rewrite_mut_Lam(lam);
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
- [`PhaseMan`](@ref mim::PhaseMan) sequences phases, optionally to a fixed point.
- [`ClosedMutPhase`](@ref mim::ClosedMutPhase) and [`NestPhase`](@ref mim::NestPhase) are traversal helpers for common whole-world inspections.

The key design idea is that MimIR phases are built around structured traversal and rewriting.
For substantial optimizations, the usual pattern is:

- compute facts with [`Analysis`](@ref mim::Analysis),
- store them in [`Analysis::lattice()`](@ref mim::Analysis::lattice),
- consume them with [`RWPhase`](@ref mim::RWPhase).
