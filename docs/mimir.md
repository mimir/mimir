# A Tour of MimIR {#mimir}

[TOC]

This tour walks through three small but complete MimIR programs.
All are compiled by the `mim` CLI and the graphs shown are generated automatically from the source via `mim --output-dot`.

The first shows how you write a classic [**SSA**](https://en.wikipedia.org/wiki/Static_single-assignment_form) computation — a counting loop — using [**continuation-passing style (CPS)**](https://en.wikipedia.org/wiki/Continuation-passing_style).
The second switches to **direct style** and adds [**higher-order functions**](https://en.wikipedia.org/wiki/Higher-order_function) and [**parametric polymorphism**](https://en.wikipedia.org/wiki/Parametric_polymorphism).
The third shows MimIR's defining trait: **types are ordinary values**, computed in the same graph as terms — the basis for [**dependent types**](https://en.wikipedia.org/wiki/Dependent_type).

Under the hood, MimIR is not a list of instructions but a **graph**.
The `let` bindings in the examples below are pure surface sugar — they only name subexpressions.
Inlining them or adding more yields the **exact same graph**, because in MimIR the graph *is* the program.

## Plugins {#mimir_plugins}

MimIR's built-in language is deliberately tiny — functions, applications, tuples, and the like.
Everything domain-specific — arithmetic, comparisons, memory, control-flow helpers, even entire [DSLs](https://en.wikipedia.org/wiki/Domain-specific_language) — lives in **plugins**.
Plugins can sit at any level you like: from low-level bit-fiddling all the way up to full-blown domain-specific operations such as [regular expressions](@ref regex).
A plugin has two halves that share one name:

- a **`.mim` file** that declares the plugin's **annexes**, and
- a shared library that registers their runtime behavior: normalizers and plugin-specific compiler [phases](@ref phases).

The examples below use the [`%core`](@ref core) plugin throughout — hence the `plugin core;` at the top of each.

### Annexes

An **annex** is any entity a plugin exports; you reference it with the `%%plugin.path` syntax, such as `%%core.wrap.add` or `%%core.select` from the [`%core`](@ref core) plugin.
Annexes come in two flavors: Axioms and other definitions.

#### Axioms

These are opaque primitives with *no* Mim definition.
Their meaning comes from the plugin's C++ side (see below), not from any Mim code.
`%%core.wrap.add` (machine addition) and `%%core.pe.is_closed` (a partial-evaluation query) are axioms.

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
every basic block becomes a **continuation** (`con`: a function that never returns).
However, in contrast to other CPS representations, functions and other binders in MimIR are **scopeless**: they are **not** explicitly nested inside one another.
This makes MimIR's take on CPS much more faithful to the original SSA formulation (see @ref mimir_scopeless):

\include "count.mim"

The correspondence is exact:

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

  `loop : Cn [I32, I32]`, `body, exit : Cn []`, and the return continuation `: Cn I32`, where `Cn A` is a function from `A` that never returns.
  A traditional basic block carries no type at all.

This is the graph MimIR builds for `count`:

@note **Reading the graphs.**
Each box is a [`Def`](@ref mim::Def) — one node of the program graph — labelled with its kind.
Boxes drawn with a clipped, **diagonal corner** are *mutable* binders (functions and other recursive nodes); plain boxes are *immutable*, [hash-consed](https://en.wikipedia.org/wiki/Hash_consing) expressions.
Edges run from a node to its operands.

@dotfile count.dot "The MimIR graph of `count`."

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

  A scoped IR typically *block-floats* functions outward before β-reduction to avoid duplicating them.
  MimIR just β-reduces on the spot.
  And this is not special to β-reduction: substitution in MimIR is a graph traversal that automatically skips any subgraph not depending on the substituted variables — unrelated functions are left untouched and shared — with no scopes to keep consistent.

- **Specialization.**

  A scoped IR *block-sinks* a function inward, nesting it deep enough that the variables it uses stay in scope.
  MimIR never needs to: a binder simply refers to whatever it refers to, wherever it sits.

And this is not limited to continuations: in MimIR **every** binder is scopeless — direct-style functions, dependent tuple types, and the rest.

What is more, there is **no control-flow graph and no dominator tree**.
The natural worry is *then how do you run any analysis?* — SSA leans on dominance for φ-placement, GVN, code motion, and the rest.
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
`succ`, `add`, `mul`, `pow`, and the assertion are all unreachable from the roots once partial evaluation has run, so [`Cleanup`](@ref mim::Cleanup) prunes them.
The graph MimIR keeps is therefore just `iter` itself:

@dotfile iter.dot "The MimIR graph of `iter` — only the `extern` root survives."

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

With type edges followed, the graph makes it literal:

@dotfile dep.dot "The MimIR graph of `Vec` and `zeros` with type edges shown."

The same variable node `n` feeds both the array **type** `«n; Nat»` and the array **value** `‹n; 0›` — a type pointing straight at a term.
Types are not an earlier, separate phase that has been erased before the IR begins; they are ordinary nodes, hash-consed, normalized, and partially evaluated alongside everything else.
This is the foundation the [`%tensor`](@ref tensor) plugin builds on: array shapes live in the types and are checked — and optimized — by the very same normalization you have already seen.
