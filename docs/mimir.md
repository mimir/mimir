# A Tour of MimIR {#mimir}

[TOC]

This tour walks through Mim's syntax, MimIR's plugin architecture, then three MimIR programs.
These are compiled by the `mim` [CLI](@ref cli) and the graphs shown are generated automatically from the source via `mim --output-dot`.

1. A classic [**SSA**](https://en.wikipedia.org/wiki/Static_single-assignment_form) computation — a counting loop — written in [**continuation-passing style (CPS)**](https://en.wikipedia.org/wiki/Continuation-passing_style).
2. A switch to **direct style** that adds [**higher-order functions**](https://en.wikipedia.org/wiki/Higher-order_function) and [**parametric polymorphism**](https://en.wikipedia.org/wiki/Parametric_polymorphism).
3. A demonstration of MimIR's defining trait: **types are ordinary values**, computed in the same graph as terms — the basis for [**dependent types**](https://en.wikipedia.org/wiki/Dependent_type).

Under the hood, MimIR is not a list of instructions but a **graph**.
The `let` bindings in the examples below are pure surface sugar — they only name subexpressions.
Inlining them or adding more yields the **exact same graph**, because in MimIR the graph *is* the program.

## Syntax

Before we dive in, here is just enough Mim syntax to read the examples.
Much of it is merely **syntactic sugar** over that graph:

- **Naming and grouping**
  - `let` names a subexpression.
  - `e where d* end` is an upside-down `let`: it attaches a block of declarations — `let`s, but also whole `lam`s/`con`s — *after* the expression that uses them.

- **Functions**
  - `lam` is a direct-style function that returns a value.
  - `con` is just sugar for a `lam` whose return type is `⊥` — a function that never returns.
  - Equivalently, `Cn A` is sugar for `A → ⊥`; this matters in the [CPS section](@ref mimir_cps) below.
  - `fun` / `return` is sugar for threading an explicit return continuation, so a CPS function still reads like an ordinary one.
  - `{}` carries implicit arguments — usually types — as in `lam id {T: *} (x: T): T = x`, where `T` is inferred at the call site (`id tt`).

- **Tuples**
  - Tuples are written `(a, b, c)`, tuple types `[A, B, C]`.
  - More generally, `[x: A, B x]` is a dependent pair type.

- **Arrays and packs**
  - Arrays are written `«i: n; T»`, or just `«n; T»` when the element type does not mention the index `i`.
    So `«n; Nat»` is the type of length-`n` arrays of naturals.
  - Packs are the corresponding homogeneous tuple terms, written `‹i: n; v›` or `‹n; v›`.
    So `‹n; 0›` is the pack of `n` zeros.

- **Indices**
  - `Idx n` is the finite type of `n` indices, so `Idx 3` is inhabited by `0₃`, `1₃`, `2₃`.
  - In particular, `Idx 2` is just `Bool`, with `0₂` = `ff` and `1₂` = `tt`.
    Hence `(f, t)#cond` simply indexes the two-element tuple `(f, t)` with the boolean `cond`.

## Plugins {#mimir_plugins}

MimIR's built-in language is deliberately tiny: functions, applications, tuples, and the like.
Everything domain-specific — arithmetic, comparisons, memory, control-flow helpers, even entire [DSLs](https://en.wikipedia.org/wiki/Domain-specific_language) — lives in **plugins**.
Plugins can sit at any level you like: from low-level bit-fiddling all the way up to full-blown domain-specific operations such as [regular expressions](@ref regex).
A plugin has two halves that share one name:

- a **`.mim` file** that declares the plugin's **annexes**, and
- a shared library that registers their runtime behavior: normalizers and plugin-specific compiler [phases](@ref phases).

The examples below use the [`%core`](@ref core) plugin throughout, hence the `plugin core;` at the top of each file.

### Annexes

An **annex** is any entity a plugin exports; you reference it with the `%%plugin.path` syntax, such as `%%core.wrap.add` or `%%core.select` from the [`%core`](@ref core) plugin.
Annexes come in two flavors: Axioms and other definitions.

#### Axioms

These are opaque primitives with *no* Mim definition.
Their meaning comes from the plugin's C++ side (see below), not from any Mim code.
`%%core.wrap.add` (machine addition) and `%%core.pe.is_closed` (which checks whether an expression contains no free variables) are treated as axioms.

Axioms get their behavior from **normalizers**: small C++ functions that live *inside* the plugin's shared library, each attached to an axiom.
Whenever a node is constructed, MimIR fires the matching normalizer **eagerly, on the fly** — so the simplified node is the only one that ever exists.
[Constant folding](https://en.wikipedia.org/wiki/Constant_folding) and [peephole](https://en.wikipedia.org/wiki/Peephole_optimization) rewrites such as `x + 0 → x` are implemented exactly this way: you never build `%%core.wrap.add (x, 0)` and optimize it away later — it collapses to `x` at construction time.

#### Other Definitions

These are ordinary Mim values, written as plain Mim.
`%%core.select`, for instance, is just
```
lam %core.select {T: *} (cond, t, f): T = (f, t)#cond;
```
Being a direct-style function, it carries the default `tt` [`filter`](@ref mim::Lam::filter), which tells MimIR to **β-reduce its applications eagerly during graph construction**.
So a call `%%core.select (a, b, c)` is inlined on the spot, collapsing to the indexed read `(c, b)#a` — the `select` itself never appears in the graph.

## SSA via CPS {#mimir_cps}

Let's start with a plain counting loop.
In C, summing `0 + 1 + … + (n-1)` looks like this:

```c
int count(int n) {
    int i = 0, acc = 0;
    while (i < n) {
        acc += i;
        i   += 1;
    }
    return acc;
}
```

A compiler lowers this into [SSA](https://en.wikipedia.org/wiki/Static_single-assignment_form) form: basic blocks, **φ-functions** for the loop-carried `i` and `acc`, and explicit branches.
In LLVM-style textual IR:

```llvm
define i32 @count(i32 %n) {
entry:
  br label %loop
loop:                                              ; preds = %entry, %body
  %i    = phi i32 [ 0, %entry ], [ %next, %body ]
  %acc  = phi i32 [ 0, %entry ], [ %sum,  %body ]
  %cond = icmp ult i32 %i, %n
  br i1 %cond, label %body, label %exit
body:
  %next = add i32 %i, 1
  %sum  = add i32 %acc, %i
  br label %loop
exit:
  ret i32 %acc
}
```

MimIR expresses the **same** program through the well-established correspondence between SSA form and CPS (see [Kelsey'95](https://dl.acm.org/doi/10.1145/202530.202532) and [Appel'98](https://dl.acm.org/doi/10.1145/278283.278285)):
every basic block becomes a **continuation** (`con` is a function that never returns).
However, in contrast to other CPS representations, functions and other binders in MimIR are **scopeless**: they are **not** explicitly nested inside one another.
This makes MimIR's take on CPS much more faithful to the original SSA formulation (see @ref mimir_scopeless):

\include "count.mim"

Correspondence between SSA form and CPS in MimIR:

| SSA                                  | MimIR / CPS                                                              |
| ------------------------------------ | ------------------------------------------------------------------------ |
| basic block                          | continuation `con`                                                       |
| φ-function at a block head           | continuation parameter (a *block argument*), e.g. `loop (i acc: I32)`    |
| φ-argument `[ v, %pred ]`            | the value passed at a call site: `loop (0I32, 0I32)`, `loop (next, sum)` |
| `br label %loop` (goto)              | tail call `loop (next, sum)`                                             |
| `br i1 %cond, %body, %exit` (branch) | `%%core.select (cond, body, exit) ()` — pick the target, then apply       |
| `ret i32 %acc` (return)              | `return acc` — call the function's return continuation                   |

The function `count` itself is written with `fun` / `return`: sugar that threads an explicit return continuation through, so it reads like an ordinary function even though it is CPS underneath.
As explained in the [Plugins](@ref mimir_plugins) section above, the *annex* `%%core.select` is an ordinary direct-style lambda, so its `tt` filter makes the branch `%%core.select (cond, body, exit)` collapse to the indexed read `(exit, body)#cond` during construction — no `select` node survives.
That is what the graph below actually shows.

CPS makes three pieces of SSA folklore explicit:

- **φ-uses live at the *end* of a block, not its head.**

  An SSA φ pretends to read its inputs at the top of its block, but each input is actually produced by a *predecessor's* terminator — φs "happen on the edge".
  In CPS the incoming values are simply the arguments passed by the tail call that *ends* each predecessor — exactly where they are computed.

- **A whole group of φs at a block head is really a [*parallel copy*](https://en.wikipedia.org/wiki/Static_single-assignment_form#Converting_out_of_SSA_form).**

  With block arguments that is plainly one argument tuple per edge, not a pile of pseudo-instructions to sequentialize.

- **Blocks have honest types.**

  `Cn A` is a function from `A` that never returns and is syntactic sugar for `A → ⊥`.
  All basic blocks and even the return point are continuations:
  - `loop : Cn [I32, I32]`
  - `body, exit : Cn []`
  - `return : Cn I32`

  In contrast, a traditional basic block carries no type at all.

This is the graph MimIR builds for `count`:

@image html count.svg "The MimIR graph of `count` (type edges elided)"

@note **Reading the graphs.**
Each box is a [`Def`](@ref mim::Def) — one node of the program graph — labelled with its kind.
Boxes drawn with a clipped, **diagonal corner** are *mutable* binders (functions and other recursive nodes); plain boxes are *immutable*, [hash-consed](https://en.wikipedia.org/wiki/Hash_consing) expressions.
Edges run from a node to its operands.

@note `extern` marks `count` as a root of the program graph.
Without it, MimIR's sea-of-nodes cleanup would prune the unused function away.

### Scopeless Binders {#mimir_scopeless}

And here is MimIR's twist.
Like traditional basic blocks — which float in the CFG, reached by label rather than by lexical position — **every** MimIR binder is *floating* and *scopeless*.
The Mim source nests `body` and `exit` inside `loop` with `where`, but that nesting exists **only in the surface syntax**:
in the graph there is no containment — `body` and `exit` are ordinary nodes the branch indexes into, and `loop`'s back-edge is just an edge from `loop` to itself.
That cycle is well-formed because binders like `loop` are **mutable** nodes (drawn with the diagonal corner); everything else stays an acyclic, [hash-consed](https://en.wikipedia.org/wiki/Hash_consing) DAG — the [sweet spot](@ref mut) MimIR hits between a fully mutable and a fully immutable IR.

This is where MimIR diverges from other functional IRs.
In a scoped IR a binder lives at a specific point in the nesting, and transformations must keep every variable properly scoped — so the compiler spends real effort moving binders around.
MimIR has no scopes: nothing has to move, and there is no lexical scoping to preserve.
Two classic chores simply vanish:

- **β-reduction.**

  In the following example `g` does not depend on `x`.
  Yet, `g` is nested inside `f`.
  For this reason, a naive β-reduction would superfluously duplicate `g` as well.
  ```ocaml
  let f x =
      let g y = y + 1 in
      g (x + 2)
  ```
  Scoped IRs therefore typically *block-float* functions independent from the substitution outward before β-reduction to avoid this problem.

  MimIR, on the other hand, just β-reduces on the spot.
  And this is not special to β-reduction: substitution in MimIR is a graph traversal that automatically skips any subgraph not depending on the substituted variables — unrelated binders are left untouched and shared — with no scopes to keep consistent.

- **Specialization.**

  In the following example, we want to specialize `f` for `z`.
  ```ocaml
  let f x y = x + y in
  let g z = (f z 1) + (f z 2)
  ```
  However, we need to *block-sink* the specialization `fz` inside `g` such that `fz`'s free variable `z` is now properly scoped:
  ```ocaml
  let g z =
    let fz y = z + y in
    (fz 1) + (fz 2)
  ```
  MimIR, on the other hand, does not need block-sinking: a binder simply refers to whatever it refers to, wherever it sits.

And this is not limited to continuations: in MimIR **every** binder is scopeless — direct-style functions, dependent tuple types, and the rest.

What is more, there is **no control-flow graph and no dominator tree**.
The natural worry is *then how do you run any analysis?* — SSA leans on dominance for φ-placement, GVN, code motion, etc.
MimIR replaces the dominator tree with the **nesting tree** induced by free variables, and answers liveness/scoping questions by querying a `Def`'s free-variable set directly — computed lazily and memoized.
These queries are always correct, even for higher-order code, and the standard SSA optimizations carry over; the construction, its complexity bounds, and its metatheory are spelled out in [*SSA without Dominance for Higher-Order Programs*](https://doi.org/10.48550/arXiv.2604.09961).

## Polymorphic Iteration {#mimir_iter}

CPS is not the only option.
MimIR equally supports **direct-style** functions that simply return a value, and these compose with higher-order functions and polymorphism.
The two styles even mix within one curried function: the outer arguments can be direct-style while the final one is a continuation, as in `con {T: *} (x: T) = …`.

The function `iter f (n, x)` applies `f` to `x` exactly `n` times.
It is [**polymorphic**](https://en.wikipedia.org/wiki/Parametric_polymorphism) — the element type `T` is just another argument, passed implicitly in `{}` — and recursive.
The `@(%%core.pe.is_closed n)` filter is a [**partial-evaluation**](https://en.wikipedia.org/wiki/Partial_evaluation) directive: whenever `n` is a constant, MimIR unrolls the recursion away at compile time:

\include "iter.mim"

The rest of the file puts `iter` to work, building a small tower of arithmetic purely by [**partial application**](https://en.wikipedia.org/wiki/Partial_application):

- `succ` adds one;
- `add x y` iterates `succ` — `x` times, starting from `y`;
- `mul x y` iterates `add x` — `y` times, starting from `0`;
- `pow x y` iterates `mul x` — `y` times, starting from `1`.

Each step hands a *partially applied* function — `add x`, `mul x` — to `iter`'s higher-order parameter `f`.
The final line is a **compile-time assertion**:

```mim
let _ = %refly.equiv.struc_eq (pow 3 5, 243);
```

Because `iter` carries the `@(%%core.pe.is_closed n)` [partial-evaluation](https://en.wikipedia.org/wiki/Partial_evaluation) filter — and every function in the tower is direct-style with the default `tt` filter — MimIR evaluates `pow 3 5` **completely during graph construction**: the whole tower unrolls to the literal `243`, and `%%refly.equiv.struc_eq` statically checks it.
A mismatch would fail the build.

Now notice what *survives*.
Only `iter` is `extern`, hence the sole [root](@ref mim::World::roots).
`succ`, `add`, `mul`, `pow`, and the assertion are all unreachable from the roots, so [`Cleanup`](@ref mim::Cleanup) prunes them.
The graph MimIR keeps is therefore just `iter` itself:

@image html iter.svg "The MimIR graph of `iter` — only the `extern` root survives (type edges elided)"

The two branches `alt` and `cons` are **floating functions**: MimIR references them as ordinary nodes selected by `cond` instead of nesting them inside `iter`, and the recursive call simply points straight back at the `iter` node.
This is MimIR's sea-of-nodes representation in action — the same machinery that expressed the counting loop above, now carrying a higher-order, polymorphic, direct-style function.

You **cannot** represent this program directly in [LLVM](https://en.wikipedia.org/wiki/LLVM) or [MLIR](https://en.wikipedia.org/wiki/MLIR_(software)): `iter` is polymorphic over `T` *and* takes a first-class function as an argument, neither of which those IRs model natively.
Getting there requires *at least partially lowering* the program first — [closure conversion](https://en.wikipedia.org/wiki/Closure_(computer_programming)) for the higher-order arguments, [type erasure](https://en.wikipedia.org/wiki/Type_erasure) or monomorphization for the polymorphism, and so on.
MimIR represents — and here even partially evaluates — it as written.

@note Unlike `count`, `iter` is a `lam` in **direct style**: it returns a `T` directly instead of threading a return continuation.
The same graph substrate represents both styles uniformly.

## Types Are Values {#mimir_dep}

So far you have seen higher-order, polymorphic, partially evaluated code in both CPS and direct style.
MimIR's defining feature ties all of that together: **types live in the same graph as terms**, built and normalized by the same machinery.
So a type can be computed by an ordinary function, depend on a runtime value, and be partially evaluated — all for free.
Watch a *type* come out of an ordinary function:

\include "dep.mim"

`Vec` is just a `lam` — but it returns `*`, the type of types, so it is a function `Nat → *`: a [type constructor](https://en.wikipedia.org/wiki/Type_constructor).
`zeros` then has a [**dependent function type**](https://en.wikipedia.org/wiki/Dependent_type): its return type `Vec n` mentions the *value* `n` of its argument.
Nothing special happens to make this work — `Vec n` is β-reduced to `«n; Nat»` during construction exactly like `%%core.select` above, even though the result is a *type* — and `%%refly.equiv.struc_eq` statically checks that `zeros 3` evaluates to `‹3; 0›`.

@image html dep.svg "The MimIR graph of `Vec` and `zeros` with type edges shown (type edges are dashed)"

The same variable node `n` feeds both the array **type** `«n; Nat»` and the array **value** `‹n; 0›` — a type pointing straight at a term.
Types are not an earlier, separate phase that has been erased before the IR begins; they are ordinary nodes, hash-consed, normalized, and partially evaluated alongside everything else.
This is the foundation the [`%tensor`](@ref tensor) plugin builds on: array shapes live in the types and are checked — and optimized — by the very same normalization you have already seen.
