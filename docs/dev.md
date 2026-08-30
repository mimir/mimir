# Developer Guide {#dev}

[TOC]

This guide summarizes the typical idioms and patterns you will want to use when working with MimIR as a developer.
It focuses on the C++ API and IR-building workflow.

## Basics

Let's jump straight into an example.

\include "examples/hello.cpp"

[`Driver`](@ref mim::Driver) is usually the first object you create.
It owns a few global facilities such as [`Flags`](@ref mim::Flags), the [`Log`](https://leissa.github.io/fe/classfe_1_1Log.html), and the current [`World`](@ref mim::World).
In this example, the log is configured to write debug output to `std::cerr`; see also @ref clidebug.

@warning Note how the [`Driver`](@ref mim::Driver) is created *outside* the `try` block.
It also owns the [`fe::SrcMap`](https://leissa.github.io/fe/classfe_1_1SrcMap.html) that holds the text of every file you lex, and an [`Error`](https://leissa.github.io/fe/classfe_1_1Error.html) only renders its `Loc`s - and their source snippets - through it.
So the [`Driver`](@ref mim::Driver) must outlive everything that may still print a diagnostic, in particular your `catch` handlers.

Next, we load the [`core`](@ref core) and [`ll`](@ref ll) plugins.
A plugin has two halves — a `.mim` file declaring its annexes and a shared library providing their runtime behavior; see [Plugins & Annexes](@ref mimir_plugins) for the overview and [Plugins](@ref plugins) for the details.
Calling `mim::ast::load_plugins` parses the `.mim` file and also loads the shared object, while the [`Driver`](@ref mim::Driver) keeps track of the resulting plugin state.

Now we can build actual code.

[`Def`](@ref mim::Def) is the base class for **all** nodes/expressions in MimIR.
Each [`Def`](@ref mim::Def) is a node in a graph managed by the [`World`](@ref mim::World).
You can think of the [`World`](@ref mim::World) as a giant hash set that owns all [`Def`s](@ref mim::Def) and provides factory methods to create them.

In this example, we construct the `main` function.
In direct style, its type looks like this:

```mim
[%mem.M 0, I32, %mem.Ptr (I32, 0)] -> [%mem.M 0, I32]
```

In [continuation-passing style (CPS)](https://en.wikipedia.org/wiki/Continuation-passing_style), the same type looks like this:

```mim
Cn [%mem.M 0, I32, %mem.Ptr (I32, 0), Cn [%mem.M 0, I32]]
```

The type `%mem.M 0` tracks side effects.
Since `main` introduces [variables](@ref mim::Var), we must create it as a **mutable** [lambda](@ref mim::Lam); see @ref mut.

The body of `main` is simple: it invokes the return continuation `ret` with `mem` and `argc`:

```mim
ret (mem, argc)
```

It is also important to mark `main` as [external](@ref mim::Def::externalize).
Otherwise, MimIR may remove it as dead code.

Finally, we [`optimize`](@ref mim::optimize) the program, emit an [LLVM assembly file](https://llvm.org/docs/LangRef.html), compile it via `clang`, and execute the generated binary with `./hello a b c` - both through `fe::sys::system`.
We then print its exit code, which should be `4`.

## Immutables vs. Mutables {#mut}

MimIR distinguishes between two kinds of [`Def`s](@ref mim::Def): _immutables_ and _mutables_.

| **Immutable**                                                          | **Mutable**                                                                                                                    |
| ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------ |
| _must be_ `const`                                                      | _may be_ non-`const`                                                                                                           |
| ops form a [DAG](https://en.wikipedia.org/wiki/Directed_acyclic_graph) | ops may be cyclic                                                                                                              |
| no recursion                                                           | may be recursive                                                                                                               |
| no [variables](@ref mim::Var)                                          | may have [variables](@ref mim::Var); use [`mim::Def::var`](@ref mim::Def::var) / [`mim::Def::has_var`](@ref mim::Def::has_var) |
| build ops first, then the actual node                                  | build the actual node first, then [`set`](@ref mim::Def::set) the ops                                                          |
| [hash-consed](https://en.wikipedia.org/wiki/Hash_consing)              | each new instance is fresh                                                                                                     |
| [`Rewriter::rewrite_imm`](@ref mim::Rewriter::rewrite_imm)             | [`Rewriter::rewrite_mut`](@ref mim::Rewriter::rewrite_mut)                                                                     |

### Immutables

Immutables are usually constructed in one step with [`World`](@ref mim::World) factory methods.
The usual pattern is: build all operands first, then create the immutable node with `w.app`, `w.tuple`, `w.pi`, `w.sigma`, and similar helpers.

For ordinary applications, [`mim::World::app`](@ref mim::World::app) is the direct building block:

```cpp
auto f   = w.annex<mem::alloc>();
auto app = w.app(w.app(f, {type, as}), mem);
```

Here, [`mim::World::annex`](@ref mim::World::annex) yields the raw axiom node itself.
That is useful when you want to partially apply a curried annex, store it, inspect it, or build the application tree step by step from C++.

If you want the full curried call in one go, prefer [`mim::World::call`](@ref mim::World::call):

```cpp
auto app    = w.call<mem::alloc>(mem);
auto mem_t  = w.call<mem::M>(0);
auto argv_t = w.call<mem::Ptr0>(w.call<mem::Ptr0>(w.type_i32()));
```

`w.call<Id>(...)` starts from `w.annex<Id>()` and completes the curried application for you, including implicit arguments.
So the rule of thumb is:

- use `w.annex<Id>()` when you need the annex itself or want manual partial application;
- use `w.call<Id>(...)` when you want the fully applied operation directly.

#### Calling Annexes

`Id` (e.g. `mem::alloc`, `mem::M`, `mem::Ptr0` above) is one of the `enum class <tag> : flags_t` types that `--output-h` generates per annex tag into a plugin's `autogen.h`; see [Generated Interfaces](@ref plugin_codegen) for how that header comes to be.
Each enumerator's value already *is* the axiom's full mangled id — 48 bits plugin, 8 bits tag, 8 bits sub — composed by [`mim::Annex::flags`](@ref mim::Annex::flags).
`w.annex(id)` just casts `id` to that `flags_t` and looks it up in the `World`'s flags-to-axiom registry.
The tag-less form used above, `w.annex<mem::alloc>()`, instead reads the id from the `Annex::Base<Id>` specialization that `autogen.h` also emits, so no plugin-specific value has to appear at the call site at all.
`w.call<Id>(...)` resolves the axiom the same way and then applies the remaining arguments one at a time; currying is driven by the axiom's own curry/trip counters, not by `Id`.

This only works because the same mangling runs on both sides: when `<plugin>.mim` is parsed for real (not bootstrapped), each `axm` declaration is attached to the `World` under that identical `Annex::flags(plugin, tag, sub)`, using the same first-occurrence tag numbering that `--output-h` used to pick the enumerator values.
So the constant baked into `Id` at compile time and the id the runtime axiom is attached under are guaranteed to agree.
The Python bindings mirror this with a generated `IntEnum` per plugin instead of a C++ `enum class`; see [Loading Plugins](@ref pyplugins) for `world.annex(...)`/`world.call(...)` from Python.

### Mutables

Mutables are built in three phases:

1. Create the mutable node with a `mut_*` factory.
2. Optionally, obtain its variable.
3. Fill in the body via [`mim::Def::set`](@ref mim::Def::set):

```cpp
auto main = w.mut_fun({mem_t, w.type_i32(), argv_t}, {mem_t, w.type_i32()})->set("main");
auto [mem, argc, argv, ret] = main->vars<4>();
main->app(false, ret, {mem, argc});
main->externalize();
```

Use [`mim::Def::externalize`](@ref mim::Def::externalize) for roots that must survive cleanup and whole-world rewrites.
Top-level entry points, generated wrapper functions, and replacement nodes for former externals all follow this pattern.

### Binders

As a more intricate example, we build a polymorphic identity function using MimIR's C++ API.

```mim
λ {T: *} (x: T): T = x
```

This example illustrates how mutables and immutables interact.
All binders must be created as mutables in order to access the [variable](@ref mim::Var) they introduce.
All other nodes can remain immutable.

\include "examples/poly.cpp"

We first build the function type `{T: *} → T → T` (stored in `pi`).
Since this type introduces the implicit variable `T` (stored in `pT`), the outer [`Pi`](@ref mim::Pi) must be created as a mutable.
Before we can retrieve this variable, we must set the domain `*` (`w.type()`).
This is also the reason why the [`Pi`](@ref mim::Pi) itself lives one universe level above, in `w.type<1>()`.
The name `"T"` is purely for debugging and has no semantic meaning.

The codomain `T → T` is built as an immutable [`Pi`](@ref mim::Pi) that refers to `pT`, and is then assigned as the codomain of `pi`.

Next, we build the actual [function](@ref mim::Lam).
The outer lambda `lamT` has type `pi` and must be mutable, since it introduces the variable `T` (stored in `lT`) of type `*` (the domain of `pi`).
Even though `pi` also introduces a `T`, `lamT`'s variable is distinct: `lT` is **not** the same variable as `pT`.

We then build the inner lambda `lamx`, which must also be mutable because it introduces the variable `x` of type `T`.

@warning The type of `lamx` is `T → T`, and must refer to `lT` (not `pT`), because `lamx` is nested inside `lamT` and thus depends on `lamT`’s variable.

Finally, we set the body of `lamx` to `x` (using a `tt` filter), and set the body of `lamT` to `lamx`, again using a `tt` filter.

#### Partial Evaluation

A `tt` filter tells MimIR to immediately β-reduce the corresponding application, giving the effect of [partial evaluation](https://en.wikipedia.org/wiki/Partial_evaluation).

```cpp
auto res = w.call(lamT, w.lit_nat(42));
```

Here we use [`World::call`](@ref mim::World::call) to automatically infer the implicit argument (`Nat`) and apply the function to `42`.
Because both lambdas were constructed with a `tt` filter, MimIR reduces the application immediately.
As a result, `res` does **not** point to a function or a residual call node, but directly to the reduced result, i.e. the literal `42`.

#### Variables

In the example above we also see that [variables](@ref mim::Var) are only created as needed.
They are **immutable**, and their sole operand is the binder where they were introduced.

```cpp
auto var = lam->var(); // create or retrieve the variable of lam

if (auto var = lam->has_var()) {
    /* only true if lam's Var already exists */
}

if (auto [lam, var] = def->isa_binder<Lam>(); lam) {
    /* only true if def is a mutable Lam whose Var already exists */
}

auto mut = var->mut(); // get the mutable binder where var was introduced
```

Which [variables](@ref mim::Var) a [`Def`](@ref mim::Def) actually refers to is answered by its [free variables](@ref free_vars); see below.

#### Free Variables {#free_vars}

Many analyses and rewrites need to know which [variables](@ref mim::Var) a [`Def`](@ref mim::Def) still refers to.
For example, a [`Def`](@ref mim::Def) can only be hoisted to an outer scope if it does not depend on the [variables](@ref mim::Var) introduced further in.

##### Local vs. Global

Recall that the operand graph is a [DAG](https://en.wikipedia.org/wiki/Directed_acyclic_graph) of immutables that is only ever "broken" by mutables (see @ref mut).
MimIR exploits this by splitting the analysis at the mutable boundary:

- [`mim::Def::local_vars`](@ref mim::Def::local_vars) / [`mim::Def::local_muts`](@ref mim::Def::local_muts) only follow **immutable** [`deps`](@ref mim::Def::deps).
  They stop as soon as they hit a mutable and record *that* mutable instead of descending into it.
  Because immutables are [hash-consed](https://en.wikipedia.org/wiki/Hash_consing), these sets are computed once at construction time, cached, and shared.
  By definition, `var->local_vars()` is `{var}` and `mut->local_muts()` is `{mut}`.
- [`mim::Def::free_vars`](@ref mim::Def::free_vars) gives the actual set of free [`Var`s](@ref mim::Var).
  It extends `local_vars()` by transitively resolving the `local_muts()`.
  Since mutables may be (mutually) recursive, this is a fixed-point iteration whose result is cached inside each mutable.

```cpp
Vars fvs  = def->free_vars(); // all Vars that still occur free in def
bool open = def->is_open();   // fvs is non-empty
bool clsd = def->is_closed(); // fvs is empty
```

@note Prefer `local_vars()` / `local_muts()` when a local answer suffices — they are free after construction — and only reach for `free_vars()` when you truly need the transitive closure.

##### Invalidation

Since mutables can be re-[`set`](@ref mim::Def::set), their cached `free_vars()` may become stale.
Each mutable therefore tracks its [`users`](@ref mim::Def::users) — the mutables that reference it — so that mutating a mutable transitively invalidates the caches of everything that (indirectly) depends on it.
You do not trigger this yourself; it happens as part of [`set`](@ref mim::Def::set) / [`unset`](@ref mim::Def::unset).

##### Scope & Nesting

Free variables also underpin MimIR's scopeless nesting queries:

- [`mim::Def::outermost_binder`](@ref mim::Def::outermost_binder) walks up `free_vars()` until the outermost enclosing binder is reached.
- [`mim::Def::nests`](@ref mim::Def::nests) answers whether a mutable statically nests another [`Def`](@ref mim::Def).
  The relation is **strict**: `f->nests(f)` is `false`, and a `def` that only uses `f`'s own [`Var`](@ref mim::Var) sits at `f`'s level and is likewise *not* nested.

##### `Dep`

When you only need a yes/no answer — *does this subtree contain **any** [`Var`](@ref mim::Var), [`Hole`](@ref mim::Hole), mutable, or [`Proxy`](@ref mim::Proxy)?* — [`mim::Def::has_dep`](@ref mim::Def::has_dep) is far cheaper than materializing `free_vars()`.
Like `local_vars()`, it is a per-node bitset (see [`mim::Dep`](@ref mim::Dep)) that only looks up to the next mutable:

```cpp
if (def->has_dep(Dep::Var)) { /* def mentions some Var before the next mutable */ }
if (!def->has_dep())        { /* def is a fully closed, ground term */ }
```

## Matching IR

MimIR provides several ways to scrutinize [`Def`s](@ref mim::Def).
Matching built-ins, i.e. subclasses of [`Def`](@ref mim::Def), works a little differently from matching [axioms](@ref mim::Axm).

### Downcasts for Built-ins {#cast_builtin}

Methods beginning with

- `isa` behave like `dynamic_cast`: they perform a runtime check and return `nullptr` if the cast fails;
- `as` behave more like `static_cast`: in `Debug` builds they assert, via the corresponding `isa`, that the cast is valid.
- `expect` behave like `as`, but - instead of merely asserting in `Debug` builds and being silently unchecked in `Release` - they *always* check via the corresponding `isa` and throw a formatted exception (via [`fe::throwf`](https://leissa.github.io/fe/namespacefe.html#a90e0f8ec6bf736dde22be99a5cfde6ca)) when the cast fails.
  Reach for `expect` (over `as`) whenever the assumption is really a claim about the incoming IR that should surface as a proper error message rather than a `Debug`-only assertion or Release-mode undefined behavior - e.g. in backends that validate an already-lowered program.

#### General Downcast

`Def::isa` / `Def::as` perform a downcast that matches both _mutables_ and _immutables_:

```cpp
void foo(const Def* def) {
    if (auto sigma = def->isa<Sigma>()) {
        // sigma has type "const Sigma*" and may be mutable or immutable
    }

    // sigma has type "const Sigma*" and may be mutable or immutable
    // asserts if def is not a Sigma
    auto sigma = def->as<Sigma>();

    // sigma has type "const Sigma*" and may be mutable or immutable
    // throws an exception (via fe::throwf) like "expected a struct type, but got '<def>'" if def is not a Sigma;
    // the argument is a description of what was expected - a plain string or a format string plus arguments
    auto s1 = def->expect<Sigma>("a struct type");
    auto s2 = def->expect<Sigma>("the operand of {}", parent);

    // the mutable counterpart, mirroring Def::as_mut; yields "Lam*"
    auto lam = def->expect_mut<Lam>("a mutable continuation");
}
```

#### Downcast to Immutables

[`mim::Def::isa_imm`](@ref mim::Def::isa_imm) / [`mim::Def::as_imm`](@ref mim::Def::as_imm) only match _immutables_:

```cpp
void foo(const Def* def) {
    if (auto imm = def->isa_imm()) {
        // imm has type "const Def*" and is immutable
    }

    if (auto sigma = def->isa_imm<Sigma>()) {
        // sigma has type "const Sigma*" and is an immutable Sigma
    }

    // sigma has type "const Sigma*" and must be an immutable Sigma
    // otherwise, this asserts
    auto sigma = def->as_imm<Sigma>();
}
```

#### Downcast to Mutables

[`mim::Def::isa_mut`](@ref mim::Def::isa_mut) / [`mim::Def::as_mut`](@ref mim::Def::as_mut) only match _mutables_.
They also remove the `const` qualifier, which gives you access to the non-`const` methods that only make sense for mutables:

```cpp
void foo(const Def* def) {
    if (auto mut = def->isa_mut()) {
        // mut has type "Def*" - note that "const" has been removed
        // This gives you access to the non-const methods:
        auto var = mut->var();
        mut->unset();
        // ...
    }

    if (auto lam = def->isa_mut<Lam>()) {
        // lam has type "Lam*"
    }

    // lam has type "Lam*" and must be a mutable Lam
    // otherwise, this asserts
    auto lam = def->as_mut<Lam>();
}
```

If the scrutinee is already a `Def*`, then `Def::isa` / `Def::as` behave the same as [`mim::Def::isa_mut`](@ref mim::Def::isa_mut) / [`mim::Def::as_mut`](@ref mim::Def::as_mut), because the missing `const` already implies mutability:

```cpp
void foo(Def* def) {
    if (auto sigma = def->isa<Sigma>()) {
        // sigma has type "Sigma*" and is mutable
    }

    if (auto sigma = def->isa_mut<Sigma>()) {
        // sigma has type "Sigma*" and is mutable
    }

    // sigma has type "Sigma*" and must be mutable
    // otherwise, this asserts
    auto sigma = def->as<Sigma>();
}
```

#### Matching Literals {#cast_lit}

Often, you want to match a [literal](@ref mim::Lit) and extract its value.
Use [`Lit::isa`](@ref mim::Lit::isa) / [`Lit::as`](@ref mim::Lit::as):

```cpp
void foo(const Def* def) {
    if (auto lit = Lit::isa(def)) {
        // lit has type "std::optional<u64>"
        // it is your responsibility to interpret the value correctly
    }

    if (auto lit = Lit::isa<f32>(def)) {
        // lit has type "std::optional<f32>"
        // it is your responsibility to interpret the value correctly
    }

    // asserts if def is not a Lit
    auto lu64 = Lit::as(def);
    auto lf32 = Lit::as<f32>(def);

    // throws an exception (via fe::throwf) like "expected an address space, but got '<def>'" if def is not a Lit
    auto a = Lit::expect(def, "an address space");
    auto f = Lit::expect<f32>(def, "a floating-point constant");
}
```

#### Summary

The following table summarizes the most important casts:

A method beginning with `expect` behaves like the `as` in the same row, but throws a formatted exception (via [`fe::throwf`](https://leissa.github.io/fe/namespacefe.html#a90e0f8ec6bf736dde22be99a5cfde6ca)) instead of asserting; it takes a description (a plain string or a format string plus arguments) of what was expected.

| `dynamic_cast` <br> `static_cast` <br> throwing                                     | Returns                                                                                                                             | If `def` is a ...                    |
| ----------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------ |
| `def->isa<Lam>()` <br> `def->as<Lam>()` <br> `def->expect<Lam>(fmt, ...)`           | `const Lam*`                                                                                                                        | [`Lam`](@ref mim::Lam)               |
| `def->isa_imm<Lam>()` <br> `def->as_imm<Lam>()`                                     | `const Lam*`                                                                                                                        | **immutable** [`Lam`](@ref mim::Lam) |
| `def->isa_mut<Lam>()` <br> `def->as_mut<Lam>()` <br> `def->expect_mut<Lam>(fmt, ...)` | `Lam*`                                                                                                                            | **mutable** [`Lam`](@ref mim::Lam)   |
| `Lit::isa(def)` <br> `Lit::as(def)` <br> `Lit::expect(def, fmt, ...)`               | [std::optional](https://en.cppreference.com/w/cpp/utility/optional)`<`[`nat_t`](@ref mim::nat_t)`>` <br> [`nat_t`](@ref mim::nat_t) | [`Lit`](@ref mim::Lit)               |
| `Lit::isa<f32>(def)` <br> `Lit::as<f32>(def)` <br> `Lit::expect<f32>(def, fmt, ...)` | [std::optional](https://en.cppreference.com/w/cpp/utility/optional)`<`[`f32`](@ref mim::f32)`>` <br> [`f32`](@ref mim::f32)         | [`Lit`](@ref mim::Lit)               |

#### Further Casts

There are also several specialized checks, usually provided as `static` methods.
They return either a pointer to the matched entity or `nullptr`.

Here are a few examples:

```cpp
void foo(const Def* def) {
    if (auto size = Idx::isa(def)) {
        // def = Idx size
    }

    if (auto lam = Lam::isa_mut_cn(def)) {
        // def is a mutable Lam of type Cn T
    }

    if (auto pi = Pi::isa_basicblock(def)) {
        // def is a Pi whose codomain is bottom and which is not returning
    }

    // yields the bit width of an Idx type, or throws a formatted exception (via fe::throwf) if it is not statically known
    // (the throwing counterpart of the std::optional-returning Idx::size2bitwidth)
    auto w = Idx::expect_bitwidth(def, "an index type of known width");
}
```

### Matching Axioms {#cast_axm}

You can match [axioms](@ref mim::Axm) via

- [`mim::Axm::isa`](@ref mim::Axm::isa), which behaves like a checked `dynamic_cast` and returns [a wrapped](@ref mim::Axm::isa) `nullptr`-like value on failure,
- [`mim::Axm::as`](@ref mim::Axm::as), which behaves like a checked `static_cast` and asserts in `Debug` builds if the match fails, or
- [`mim::Axm::expect`](@ref mim::Axm::expect), which - like the other `expect` helpers - throws a formatted exception (via [`fe::throwf`](https://leissa.github.io/fe/namespacefe.html#a90e0f8ec6bf736dde22be99a5cfde6ca)) instead of asserting: `Axm::expect<mem::Ptr>(def, "a %mem.Ptr")`.

The result is a `mim::Axm::isa<Id, D>`, which wraps a `const D*`.
Here, `Id` is the enum corresponding to the [matched axiom tag](@ref anatomy), and `D` is usually an [`App`](@ref mim::App), because most [axioms](@ref mim::Axm) inhabit a [function type](@ref mim::Pi).
In other cases, it may wrap a plain [`Def`](@ref mim::Def) or some other subclass.

By default, MimIR assumes that an [axiom](@ref mim::Axm) becomes "active" when its final curried argument is applied.
For example, [matching](@ref mim::Axm::isa) `%%mem.load` only succeeds on the final [`App`](@ref mim::App) of the curried call

```mim
%mem.load (T, as) (mem, ptr)
```

whereas

```mim
%mem.load (T, as)
```

does **not** match.

In this example, the wrapped [`App`](@ref mim::App) refers to the final application, so:

- [`mim::App::arg`](@ref mim::App::arg) is `(mem, ptr)`, and
- [`mim::App::callee`](@ref mim::App::callee) is `%%mem.load (T, as)`.

Use [`mim::App::decurry`](@ref mim::App::decurry) if you want direct access to the preceding application.
See the examples below.

If you design an [axiom](@ref mim::Axm) that returns a function, you can [fine-tune the trigger point](@ref normalization) of [`mim::Axm::isa`](@ref mim::Axm::isa) / [`mim::Axm::as`](@ref mim::Axm::as).

#### Without Subtags

To match an [axiom](@ref mim::Axm) **without** subtags, such as `%%mem.load`, use:

```cpp
void foo(const Def* def) {
    if (auto load = Axm::isa<mem::load>(def)) {
        auto [mem, ptr]            = load->args<2>();
        auto [pointee, addr_space] = load->decurry()->args<2>();
    }

    // def must match mem::load - otherwise, this asserts
    auto load = Axm::as<mem::load>(def);

    // def must match mem::load - otherwise, this throws a formatted exception (via fe::throwf)
    auto ld = Axm::expect<mem::load>(def, "a %mem.load");
}
```

#### With Subtags

To match an [axiom](@ref mim::Axm) **with** subtags, such as `%%core.wrap`, use:

```cpp
void foo(const Def* def) {
    if (auto wrap = Axm::isa<core::wrap>(def)) {
        auto [a, b] = wrap->args<2>();
        auto mode   = wrap->decurry()->arg();
        switch (wrap.id()) {
            case core::wrap::add: // ...
            case core::wrap::sub: // ...
            case core::wrap::mul: // ...
            case core::wrap::shl: // ...
        }
    }

    if (auto add = Axm::isa(core::wrap::add, def)) {
        auto [a, b] = add->args<2>();
        auto mode   = add->decurry()->arg();
    }

    // def must match core::wrap - otherwise, this asserts
    auto wrap = Axm::as<core::wrap>(def);

    // def must match core::wrap::add - otherwise, this asserts
    auto add = Axm::as(core::wrap::add, def);
}
```

#### Summary

The following table summarizes the most important axiom matches:

| `dynamic_cast` <br> `static_cast`                           | Returns                                                                                                                     | If `def` is a ...                           |
| ----------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------- |
| `isa<mem::load>(def)` <br> `as<mem::load>(def)`             | [`mim::Axm::isa`](@ref mim::Axm::isa) specialized for [`mem::load`](@ref mim::plug::mem::load) and [`App`](@ref mim::App)   | final curried `%%mem.load` application      |
| `isa<core::wrap>(def)` <br> `as<core::wrap>(def)`           | [`mim::Axm::isa`](@ref mim::Axm::isa) specialized for [`core::wrap`](@ref mim::plug::core::wrap) and [`App`](@ref mim::App) | final curried `%%core.wrap` application     |
| `isa(core::wrap::add, def)` <br> `as(core::wrap::add, def)` | [`mim::Axm::isa`](@ref mim::Axm::isa) specialized for [`core::wrap`](@ref mim::plug::core::wrap) and [`App`](@ref mim::App) | final curried `%%core.wrap.add` application |

## Working with Indices

In MimIR, there are essentially **three** ways to talk about the number of elements of something.

### Arity

The [arity](@ref mim::Def::arity) is the number of elements available to [extract](@ref mim::Extract) / [insert](@ref mim::Insert).
This number may itself be dynamic, for example in `‹n; 0›`.

### Projs

[`mim::Def::num_projs`](@ref mim::Def::num_projs) equals [`mim::Def::arity`](@ref mim::Def::arity) if the arity is a [`mim::Lit`](@ref mim::Lit).
Otherwise, it yields `1`.

This concept mainly exists in the C++ API to give you the illusion of n-ary structure, e.g.

```cpp
for (auto dom : pi->doms()) { /*...*/ }
for (auto var : lam->vars()) { /*...*/ }
```

Internally, however, all functions still have exactly one domain and one codomain.

#### Thresholded Variants

There are also thresholded variants prefixed with `t`, which take [`mim::Flags::scalarize_threshold`](@ref mim::Flags::scalarize_threshold) (`--scalarize-threshold`) into account.

[`mim::Def::num_tprojs`](@ref mim::Def::num_tprojs) behaves like [`mim::Def::num_projs`](@ref mim::Def::num_projs), but returns `1` if the arity exceeds the threshold.
Similarly, [`mim::Def::tproj`](@ref mim::Def::tproj), [`mim::Def::tprojs`](@ref mim::Def::tprojs), [`mim::Lam::tvars`](@ref mim::Lam::tvars), and related methods follow the same rule.

**See also:**

- @ref proj "Def::proj"
- @ref var "Def::var"
- @ref pi_dom "Pi::dom"
- @ref lam_dom "Lam::dom"
- @ref app_arg "App::arg"

### Shape

TODO

### Summary

| Expression         | Class                    | [arity](@ref mim::Def::arity) | [`num_projs`](@ref mim::Def::num_projs) | [`num_tprojs`](@ref mim::Def::num_tprojs) |
| ------------------ | ------------------------ | ----------------------------- | ------------------------------------- | --------------------------------------- |
| `(0, 1, 2)`        | [`Tuple`](@ref mim::Tuple) | `3`                           | `3`                                   | `3`                                     |
| `‹3; 0›`           | [`Pack`](@ref mim::Pack)   | `3`                           | `3`                                   | `3`                                     |
| `‹n; 0›`           | [`Pack`](@ref mim::Pack)   | `n`                           | `1`                                   | `1`                                     |
| `[Nat, Bool, Nat]` | [`Sigma`](@ref mim::Sigma) | `3`                           | `3`                                   | `3`                                     |
| `«3; Nat»`         | [`Arr`](@ref mim::Arr)     | `3`                           | `3`                                   | `3`                                     |
| `«n; Nat»`         | [`Arr`](@ref mim::Arr)     | `n`                           | `1`                                   | `1`                                     |
| `x: [Nat, Bool]`   | [`Var`](@ref mim::Var)     | `2`                           | `2`                                   | `2`                                     |
| `‹32; 0›`          | [`Pack`](@ref mim::Pack)   | `32`                          | `32`                                  | `1`                                     |

The last line assumes `mim::Flags::scalarize_threshold = 32`.

## Iterating over the Program

There are several ways to iterate over a MimIR program.
Which one is best depends on what you want to do and how much structure you need during the traversal.

The simplest approach is to start from [`World::annexes`](@ref mim::World::annexes) and [`World::externals`](@ref mim::World::externals) and recursively visit [`Def::deps`](@ref mim::Def::deps).
Oftentimes, you can use [`World::roots`](@ref mim::World::roots) if you don't need to distinguish between annexes and externals:

```cpp
void visit(DefSet& done, const Def* def) {
    if (!done.emplace(def).second) return;

    do_sth(def);

    for (auto op : def->deps())
        visit(done, op);
}

void visit() {
    DefSet done;

    for (auto def : world.roots())
        visit(done, def);
}
```

In practice, though, you will usually want to use the [phase](@ref phases) infrastructure instead.
