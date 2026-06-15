# Developer Guide {#dev}

[TOC]

This guide summarizes the typical idioms and patterns you will want to use when working with MimIR as a developer.

## Basics

Let's jump straight into an example.

\include "examples/hello.cpp"

[Driver](@ref mim::Driver) is usually the first class you create.
It owns a few global facilities such as [Flags](@ref mim::Flags), the [Log](@ref mim::Log), and the current [World](@ref mim::World).
In this example, the log is configured to write debug output to `std::cerr`; see also @ref clidebug.

Next, we load the [core](@ref core), and [ll](@ref ll) plugins.
A plugin consists of two parts:

1. a shared object (`.so`/`.dll`), and
2. a `.mim` file.

The shared object contains [passes](@ref mim::Pass), [normalizers](@ref mim::Axm::normalizer), and similar runtime components.
The `.mim` file contains [axiom](@ref mim::Axm) declarations and links normalizers to their corresponding [axioms](@ref mim::Axm).
Calling mim::ast::load_plugins parses the `.mim` file and also loads the shared object, while the [Driver](@ref mim::Driver) keeps track of the resulting plugin state.

Now we can build actual code.

[Def](@ref mim::Def) is the base class for **all** nodes/expressions in MimIR.
Each [Def](@ref mim::Def) is a node in a graph managed by the [World](@ref mim::World).
You can think of the [World](@ref mim::World) as a giant hash set that owns all [Defs](@ref mim::Def) and provides factory methods to create them.

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

Finally, we [optimize](@ref mim::optimize) the program, emit an [LLVM assembly file](https://llvm.org/docs/LangRef.html), compile it [via](@ref mim::sys::system) `clang`, and [execute](@ref mim::sys::system) the generated binary with `./hello a b c`.
We then print its exit code, which should be `4`.

## Immutables vs. Mutables {#mut}

MimIR distinguishes between two kinds of [Defs](@ref mim::Def): _immutables_ and _mutables_.

| **Immutable**                                                          | **Mutable**                                                                                                                |
| ---------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- |
| _must be_ `const`                                                      | _may be_ non-`const`                                                                                                       |
| ops form a [DAG](https://en.wikipedia.org/wiki/Directed_acyclic_graph) | ops may be cyclic                                                                                                          |
| no recursion                                                           | may be recursive                                                                                                           |
| no [variables](@ref mim::Var)                                          | may have [variables](@ref mim::Var); use [mim::Def::var](@ref mim::Def::var) / [mim::Def::has_var](@ref mim::Def::has_var) |
| build ops first, then the actual node                                  | build the actual node first, then [set](@ref mim::Def::set) the ops                                                        |
| [hash-consed](https://en.wikipedia.org/wiki/Hash_consing)              | each new instance is fresh                                                                                                 |
| [Def::rebuild](@ref mim::Def::rebuild)                                 | [Def::stub](@ref mim::Def::stub)                                                                                           |

### Immutables

Immutables are usually constructed in one step with [World](@ref mim::World) factory methods.
The usual pattern is: build all operands first, then create the immutable node with `w.app`, `w.tuple`, `w.pi`, `w.sigma`, and similar helpers.

For ordinary applications, [mim::World::app](@ref mim::World::app) is the direct building block:

```cpp
auto f   = w.annex<mem::alloc>();
auto app = w.app(w.app(f, {type, as}), mem);
```

Here, [mim::World::annex](@ref mim::World::annex) yields the raw axiom node itself.
That is useful when you want to partially apply a curried annex, store it, inspect it, or build the application tree step by step from C++.

If you want the full curried call in one go, prefer [mim::World::call](@ref mim::World::call):

```cpp
auto app    = w.call<mem::alloc>(mem);
auto mem_t  = w.call<mem::M>(0);
auto argv_t = w.call<mem::Ptr0>(w.call<mem::Ptr0>(w.type_i32()));
```

`w.call<Id>(...)` starts from `w.annex<Id>()` and completes the curried application for you, including implicit arguments.
So the rule of thumb is:

- use `w.annex<Id>()` when you need the annex itself or want manual partial application;
- use `w.call<Id>(...)` when you want the fully applied operation directly.

### Mutables

Mutables are built in three phases:

1. Create the mutable node with a `mut_*` factory or a [stub](@ref mim::Def::stub).
2. Optionally, obtain its variable.
3. Fill in the body via [mim::Def::set](@ref mim::Def::set):

```cpp
auto main = w.mut_fun({mem_t, w.type_i32(), argv_t}, {mem_t, w.type_i32()})->set("main");
auto [mem, argc, argv, ret] = main->vars<4>();
main->app(false, ret, {mem, argc});
main->externalize();
```

Use [mim::Def::externalize](@ref mim::Def::externalize) for roots that must survive cleanup and whole-world rewrites.
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

## Matching IR

MimIR provides several ways to scrutinize [Defs](@ref mim::Def).
Matching built-ins, i.e. subclasses of [Def](@ref mim::Def), works a little differently from matching [axioms](@ref mim::Axm).

### Downcasts for Built-ins {#cast_builtin}

Methods beginning with

- `isa` behave like `dynamic_cast`: they perform a runtime check and return `nullptr` if the cast fails;
- `as` behave more like `static_cast`: in `Debug` builds they assert, via the corresponding `isa`, that the cast is valid.

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
}
```

#### Downcast to Immutables

[mim::Def::isa_imm](@ref mim::Def::isa_imm) / [mim::Def::as_imm](@ref mim::Def::as_imm) only match _immutables_:

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

[mim::Def::isa_mut](@ref mim::Def::isa_mut) / [mim::Def::as_mut](@ref mim::Def::as_mut) only match _mutables_.
They also remove the `const` qualifier, which gives you access to the non-`const` methods that only make sense for mutables:

```cpp
void foo(const Def* def) {
    if (auto mut = def->isa_mut()) {
        // mut has type "Def*" - note that "const" has been removed
        // This gives you access to the non-const methods:
        auto var  = mut->var();
        auto stub = mut->stub(world, type, debug);
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

If the scrutinee is already a `Def*`, then `Def::isa` / `Def::as` behave the same as [mim::Def::isa_mut](@ref mim::Def::isa_mut) / [mim::Def::as_mut](@ref mim::Def::as_mut), because the missing `const` already implies mutability:

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
Use [Lit::isa](@ref mim::Lit::isa) / [Lit::as](@ref mim::Lit::as):

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
}
```

#### Summary

The following table summarizes the most important casts:

| `dynamic_cast` <br> `static_cast`               | Returns                                                                                                                         | If `def` is a ...                  |
| ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| `def->isa<Lam>()` <br> `def->as<Lam>()`         | `const Lam*`                                                                                                                    | [Lam](@ref mim::Lam)               |
| `def->isa_imm<Lam>()` <br> `def->as_imm<Lam>()` | `const Lam*`                                                                                                                    | **immutable** [Lam](@ref mim::Lam) |
| `def->isa_mut<Lam>()` <br> `def->as_mut<Lam>()` | `Lam*`                                                                                                                          | **mutable** [Lam](@ref mim::Lam)   |
| `Lit::isa(def)` <br> `Lit::as(def)`             | [std::optional](https://en.cppreference.com/w/cpp/utility/optional)`<`[nat_t](@ref mim::nat_t)`>` <br> [nat_t](@ref mim::nat_t) | [Lit](@ref mim::Lit)               |
| `Lit::isa<f32>(def)` <br> `Lit::as<f32>(def)`   | [std::optional](https://en.cppreference.com/w/cpp/utility/optional)`<`[f32](@ref mim::f32)`>` <br> [f32](@ref mim::f32)         | [Lit](@ref mim::Lit)               |

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
}
```

### Matching Axioms {#cast_axm}

You can match [axioms](@ref mim::Axm) via

- [mim::Axm::isa](@ref mim::Axm::isa), which behaves like a checked `dynamic_cast` and returns [a wrapped](@ref mim::Axm::isa) `nullptr`-like value on failure, or
- [mim::Axm::as](@ref mim::Axm::as), which behaves like a checked `static_cast` and asserts in `Debug` builds if the match fails.

The result is a `mim::Axm::isa<Id, D>`, which wraps a `const D*`.
Here, `Id` is the enum corresponding to the [matched axiom tag](@ref anatomy), and `D` is usually an [App](@ref mim::App), because most [axioms](@ref mim::Axm) inhabit a [function type](@ref mim::Pi).
In other cases, it may wrap a plain [Def](@ref mim::Def) or some other subclass.

By default, MimIR assumes that an [axiom](@ref mim::Axm) becomes "active" when its final curried argument is applied.
For example, [matching](@ref mim::Axm::isa) `%%mem.load` only succeeds on the final [App](@ref mim::App) of the curried call

```mim
%mem.load (T, as) (mem, ptr)
```

whereas

```mim
%mem.load (T, as)
```

does **not** match.

In this example, the wrapped [App](@ref mim::App) refers to the final application, so:

- [mim::App::arg](@ref mim::App::arg) is `(mem, ptr)`, and
- [mim::App::callee](@ref mim::App::callee) is `%%mem.load (T, as)`.

Use [mim::App::decurry](@ref mim::App::decurry) if you want direct access to the preceding application.
See the examples below.

If you design an [axiom](@ref mim::Axm) that returns a function, you can [fine-tune the trigger point](@ref normalization) of [mim::Axm::isa](@ref mim::Axm::isa) / [mim::Axm::as](@ref mim::Axm::as).

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

[mim::Def::num_projs](@ref mim::Def::num_projs) equals [mim::Def::arity](@ref mim::Def::arity) if the arity is a [mim::Lit](@ref mim::Lit).
Otherwise, it yields `1`.

This concept mainly exists in the C++ API to give you the illusion of n-ary structure, e.g.

```cpp
for (auto dom : pi->doms()) { /*...*/ }
for (auto var : lam->vars()) { /*...*/ }
```

Internally, however, all functions still have exactly one domain and one codomain.

#### Thresholded Variants

There are also thresholded variants prefixed with `t`, which take [mim::Flags::scalarize_threshold](@ref mim::Flags::scalarize_threshold) (`--scalarize-threshold`) into account.

[mim::Def::num_tprojs](@ref mim::Def::num_tprojs) behaves like [mim::Def::num_projs](@ref mim::Def::num_projs), but returns `1` if the arity exceeds the threshold.
Similarly, [mim::Def::tproj](@ref mim::Def::tproj), [mim::Def::tprojs](@ref mim::Def::tprojs), [mim::Lam::tvars](@ref mim::Lam::tvars), and related methods follow the same rule.

**See also:**

- @ref proj "Def::proj"
- @ref var "Def::var"
- @ref pi_dom "Pi::dom"
- @ref lam_dom "Lam::dom"
- @ref app_arg "App::arg"

### Shape

TODO

### Summary

| Expression         | Class                    | [arity](@ref mim::Def::arity) | [num_projs](@ref mim::Def::num_projs) | [num_tprojs](@ref mim::Def::num_tprojs) |
| ------------------ | ------------------------ | ----------------------------- | ------------------------------------- | --------------------------------------- |
| `(0, 1, 2)`        | [Tuple](@ref mim::Tuple) | `3`                           | `3`                                   | `3`                                     |
| `‹3; 0›`           | [Pack](@ref mim::Pack)   | `3`                           | `3`                                   | `3`                                     |
| `‹n; 0›`           | [Pack](@ref mim::Pack)   | `n`                           | `1`                                   | `1`                                     |
| `[Nat, Bool, Nat]` | [Sigma](@ref mim::Sigma) | `3`                           | `3`                                   | `3`                                     |
| `«3; Nat»`         | [Arr](@ref mim::Arr)     | `3`                           | `3`                                   | `3`                                     |
| `«n; Nat»`         | [Arr](@ref mim::Arr)     | `n`                           | `1`                                   | `1`                                     |
| `x: [Nat, Bool]`   | [Var](@ref mim::Var)     | `2`                           | `2`                                   | `2`                                     |
| `‹32; 0›`          | [Pack](@ref mim::Pack)   | `32`                          | `32`                                  | `1`                                     |

The last line assumes `mim::Flags::scalarize_threshold = 32`.

## Iterating over the Program

There are several ways to iterate over a MimIR program.
Which one is best depends on what you want to do and how much structure you need during the traversal.

The simplest approach is to start from [World::annexes](@ref mim::World::annexes) and [World::externals](@ref mim::World::externals) and recursively visit [Def::deps](@ref mim::Def::deps).
Oftentimes, you can use [World::roots](@ref mim::World::roots) if you don't need to distinguish between annexes and externals:

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
