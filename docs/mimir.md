# A Tour of MimIR {#mimir}

[TOC]

This tour walks through two small but complete MimIR programs.
Both are compiled by the `mim` CLI and the graphs shown are generated automatically from the source via `mim --output-dot`.

The first example shows how you write a classic **SSA** computation — a counting loop — using **continuation-passing style (CPS)**.
The second example switches to **direct style** and adds **higher-order functions** and **parametric polymorphism**.
Together they cover the two complementary ways of expressing control and data flow in MimIR.

Under the hood, MimIR is not a list of instructions but a **graph**.
The `let` bindings in the examples below are pure surface sugar — they only name subexpressions.
Inlining them or adding more yields the **exact same graph**, because in MimIR the graph *is* the program.

## Counting in SSA — via CPS {#mimir_cps}

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

In [SSA](https://en.wikipedia.org/wiki/Static_single-assignment_form), the loop-carried variables `i` and `acc` become **φ-nodes** at the loop header.
In MimIR, the loop header is simply a **continuation** `loop` whose parameters `i` and `acc` *are* those φ-nodes — no separate φ-instruction needed:

\include "count.mim"

The branch picks one of two **floating continuations** with `(exit, body)#cond ()`:

- `cond = %core.icmp.ul (i, n)` is the loop condition (`i < n`).
- `#` indexes the pair `(exit, body)` with `cond`, and the result is immediately applied to `()`.
- `body` does one iteration and **jumps back** to `loop` with the updated `i` and `acc` — this back-edge is the entire loop.
- `exit` hands the accumulated `acc` to `return`, the function's return continuation.

This is exactly the graph MimIR builds for `count`:

@dotfile count.dot "The MimIR graph of `count`."

Notice there is **no control-flow graph and no dominator tree**.
The loop is just the continuation `loop` referencing itself through the `body` back-edge, and the two branches are ordinary nodes selected by `cond`.
A `con` (continuation) is a function that never returns; in Mim its type is written `Cn …`.
`fun` / `return` is sugar that threads an explicit return continuation through, so `count` reads like an ordinary function even though it is CPS underneath.

@note `extern` marks `count` as a root of the program graph.
Without it, MimIR's sea-of-nodes cleanup would prune the unused function away.

## Direct Style, Higher-Order, Polymorphic {#mimir_iter}

CPS is not the only option.
MimIR equally supports **direct-style** functions that simply return a value, and these compose with higher-order functions and polymorphism.

`iter f (n, x)` applies `f` to `x` exactly `n` times.
It is **polymorphic** — the element type `T` is just another argument, passed implicitly in `{}` — and recursive.
The `@(%core.pe.is_closed n)` filter is a **partial-evaluation** directive: whenever `n` is a constant, MimIR unrolls the recursion away at compile time:

\include "iter.mim"

And here is the graph MimIR builds for `iter`:

@dotfile iter.dot "The MimIR graph of `iter`."

Notice that the two branches `alt` and `cons` are again **floating functions**: MimIR references them as ordinary nodes selected by `cond` instead of nesting them inside `iter`, and the recursive call simply points back at the `iter` node itself.
This is MimIR's sea-of-nodes representation in action — the same machinery that expressed the counting loop above, now carrying a higher-order, polymorphic, direct-style function.

@note Unlike `count`, `iter` is a `lam` in **direct style**: it returns a `T` directly instead of threading a return continuation.
The same graph substrate represents both styles uniformly.

## Where to Go Next

- The [Language Reference](@ref langref) specifies Mim's surface syntax in full.
- The [Developer Guide](@ref dev) shows how to build and manipulate these graphs from C++.
- [Plugins](@ref plugins) explains how axioms such as `%core.icmp.ul` and `%core.pe.is_closed` are defined and made executable.
