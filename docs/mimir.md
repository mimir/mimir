# A Tour of MimIR {#mimir}

[TOC]

This tour walks through two small but complete MimIR programs.
Both are compiled by the `mim` CLI and the graphs shown are generated automatically from the source via `mim --output-dot`.

The first example shows how you write a classic [**SSA**](https://en.wikipedia.org/wiki/Static_single-assignment_form) computation — a counting loop — using [**continuation-passing style (CPS)**](https://en.wikipedia.org/wiki/Continuation-passing_style).
The second example switches to **direct style** and adds [**higher-order functions**](https://en.wikipedia.org/wiki/Higher-order_function) and [**parametric polymorphism**](https://en.wikipedia.org/wiki/Parametric_polymorphism).
Together they cover the two complementary ways of expressing control and data flow in MimIR.

Under the hood, MimIR is not a list of instructions but a **graph**.
The `let` bindings in the examples below are pure surface sugar — they only name subexpressions.
Inlining them or adding more yields the **exact same graph**, because in MimIR the graph *is* the program.

## Plugins & Annexes {#mimir_plugins}

MimIR's built-in language is deliberately tiny — functions, applications, tuples, and the like.
Everything domain-specific — arithmetic, comparisons, memory, control-flow helpers, even entire [DSLs](https://en.wikipedia.org/wiki/Domain-specific_language) — lives in **plugins**.
A plugin has two halves that share one name:

- a **`.mim` file** that declares the plugin's **annexes**, and
- a shared library that registers their runtime behavior: normalizers, [phases](@ref phases), and code-generation backends.

An **annex** is any entity a plugin exports; you reference it with the `%plugin.path` syntax, such as `%core.wrap.add` or `%core.select` from the [`core`](@ref core) plugin.
Annexes come in two flavors:

- **Axioms** — opaque primitives with *no* Mim definition.
  Their meaning is supplied by C++ normalizers and backends.
  `%core.wrap.add` (machine addition) and `%core.pe.is_closed` (a partial-evaluation query) are axioms.
- **Definitions** — ordinary Mim values, written as plain Mim.
  `%core.select`, for instance, is just `lam %core.select {T: *} (cond, t, f): T = (f, t)#cond`, and behaves like any other function.

Both examples below use the [`core`](@ref core) plugin throughout — hence the `plugin core;` at the top of each.
See [Plugins](@ref plugins) for the full story, including how to write your own.

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

MimIR expresses the **same** program in CPS — every basic block becomes a **continuation** (`con`: a function that never returns):

\include "count.mim"

The correspondence is exact:

| SSA                                  | MimIR / CPS                                                              |
| ------------------------------------ | ------------------------------------------------------------------------ |
| basic block                          | continuation `con`                                                       |
| φ-function at a block head           | continuation parameter (a *block argument*), e.g. `loop (i acc: I32)`    |
| φ-argument `[ v, %pred ]`            | the value passed at a call site: `loop (0I32, 0I32)`, `loop (next, sum)` |
| `br label %loop` (goto)              | tail call `loop (next, sum)`                                             |
| `br i1 %cond, %body, %exit` (branch) | `%core.select (cond, body, exit) ()` — pick the target, then apply       |
| `ret i32 %acc` (return)              | `return acc` — call the function's return continuation                   |

The function `count` itself is written with `fun` / `return`: sugar that threads an explicit return continuation through, so it reads like an ordinary function even though it is CPS underneath.
The *annex* `%core.select` is not built in — it is an ordinary lambda defined in the [`core`](@ref core) plugin: `lam %core.select {T: *} (cond, t, f): T = (f, t)#cond`.
Like every direct-style function it carries the default `tt` [`filter`](@ref mim::Lam::filter), so the call `%core.select (cond, body, exit)` is **β-reduced away during graph construction**, leaving just the indexed read `(exit, body)#cond` — no `select` node survives.
That is what the graph below actually shows.

CPS makes two pieces of SSA folklore explicit:

- **φ-uses live at the *end* of a block, not its head.**
  An SSA φ pretends to read its inputs at the top of its block, but each input is actually produced by a *predecessor's* terminator — φs "happen on the edge".
  In CPS the incoming values are simply the arguments passed by the tail call that *ends* each predecessor — exactly where they are computed.
  And a whole group of φs at a block head is really a [**parallel copy**](https://en.wikipedia.org/wiki/Static_single-assignment_form#Converting_out_of_SSA_form); with block arguments that is plainly one argument tuple per edge, not a pile of pseudo-instructions to sequentialize.
- **Blocks have honest types.**
  `loop : Cn [I32, I32]`, `body, exit : Cn []`, and the return continuation `: Cn I32`, where `Cn A` is a function from `A` that never returns.
  A traditional basic block carries no type at all.

This is the graph MimIR builds for `count`:

@dotfile count.dot "The MimIR graph of `count`."

And here is MimIR's twist.
Like traditional basic blocks — which float in the CFG, reached by label rather than by lexical position — **every** MimIR binder is *floating* and *scopeless*.
The Mim source nests `body` and `exit` inside `loop` with `where`, but that nesting exists **only in the surface syntax**:
in the graph there is no containment — `body` and `exit` are ordinary nodes the branch indexes into, and `loop`'s back-edge is just an edge from `loop` to itself.
There is **no control-flow graph and no dominator tree**; free-variable nesting replaces both.

@note `extern` marks `count` as a root of the program graph.
Without it, MimIR's sea-of-nodes cleanup would prune the unused function away.

## Direct Style, Higher-Order, Polymorphic {#mimir_iter}

CPS is not the only option.
MimIR equally supports **direct-style** functions that simply return a value, and these compose with higher-order functions and polymorphism.

`iter f (n, x)` applies `f` to `x` exactly `n` times.
It is [**polymorphic**](https://en.wikipedia.org/wiki/Parametric_polymorphism) — the element type `T` is just another argument, passed implicitly in `{}` — and recursive.
The `@(%core.pe.is_closed n)` filter is a [**partial-evaluation**](https://en.wikipedia.org/wiki/Partial_evaluation) directive: whenever `n` is a constant, MimIR unrolls the recursion away at compile time:

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

Because `iter` carries the `@(%core.pe.is_closed n)` [partial-evaluation](https://en.wikipedia.org/wiki/Partial_evaluation) filter — and every function in the tower is direct-style with the default `tt` filter — MimIR evaluates `pow 3 5` **completely during graph construction**: the whole tower unrolls to the literal `243`, and `%refly.equiv.struc_eq` statically checks it.
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

## Where to Go Next

- The [Language Reference](@ref langref) specifies Mim's surface syntax in full.
- The [Developer Guide](@ref dev) shows how to build and manipulate these graphs from C++.
- [Plugins](@ref plugins) explains how axioms such as `%core.icmp.ul` and `%core.pe.is_closed` are defined and made executable.
