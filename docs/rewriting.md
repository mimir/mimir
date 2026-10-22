# Rewriting {#rewriting}

[TOC]

In MimIR, rewriting means rebuilding an IR graph, usually into a target [`World`](@ref mim::World), while preserving sharing and handling mutable nodes correctly.
The rewriting infrastructure is used for tasks like copying IR between worlds, rebuilding terms after substitutions, and reconstructing terms after local changes.
It is also the foundation of several [phase](@ref phases) families.

## Rewriter {#rewriter}

[`Rewriter`](@ref mim::Rewriter) walks a graph of [`Def`](@ref mim::Def)s and constructs corresponding nodes in a target world.
It memoizes the mapping from old to new nodes, so a node referenced multiple times is rebuilt once and shared - rewriting is graph-aware rather than tree-only.

The same map makes rewriting terminate on recursive structures.
A mutable is rebuilt by first creating a stub, mapping old mutable to stub, and only then rewriting and filling its operands.
A recursive reference thus resolves to the stub instead of recursing forever.

```cpp
auto rw      = mim::Rewriter(new_world);
auto new_def = rw.rewrite(old_def);
```

### Scopes {#rewriter_scopes}

A rewrite often has to map an old node to something that is only valid *locally*: a binder's [`Var`](@ref mim::Var) to a fresh representative, a loop index to one particular literal, and so on.
For this, the map is a stack of [scopes](@ref mim::Rewriter::push).
[`push(old_vars, new_vars)`](@ref mim::Rewriter::push) opens a scope that owns two sets of [`Vars`](@ref mim::Vars) - one of the old world, one of the new - and [`pop()`](@ref mim::Rewriter::pop) discards it together with all of its mappings.
The bottom scope - the *root* - is always there.

A scope does not take *every* mapping recorded while it is open, though.
[`map(old, new)`](@ref mim::Rewriter::map) files the mapping into the innermost scope whose old `Vars` `old` mentions or whose new `Vars` `new` mentions as free variables; any other mapping goes into the root.
So only what actually depends on the local binding dies with the scope, while everything context-free stays shared across sibling scopes and beyond.
For instance, [`rewrite_mut_Seq`](@ref mim::Rewriter::rewrite_mut_Seq) scalarizes `«i: n; body»` by rewriting `body` once per `i`, each time under

```cpp
push(Vars(var));
map(var, world().lit_idx(n, i));
auto new_op = rewrite(seq->body());
pop();
```

Only those parts of `body` that mention `var` are rebuilt for every `i`; the rest is rewritten once and found in the root on the next iteration.

Dependencies propagate:

- If a mutable binder is mapped into a scope, its own `Var` joins that scope's old `Vars`, so whatever its body builds from that `Var` lands there, too.
- [`bind_old(var)`](@ref mim::Rewriter::bind_old) and [`bind_new(var)`](@ref mim::Rewriter::bind_new) add a `Var` to the innermost scope by hand.
  The latter is for a fresh binder of the *new* world whose `Var` should keep new nodes local - e.g. the continuation `cps::phase::Conv` introduces when lifting a call.

[`lookup()`](@ref mim::Rewriter::lookup) searches the scopes from innermost to root.

### Nested Rewriters {#rewriter_nested}

Sometimes the *same* old node must be rewritten differently depending on where it occurs.
[`Scalarize`](@ref mim::Scalarize), for example, flattens a function type `Cn [[A, B], C]` to `Cn [A, B, C]` everywhere - except in what the signature of an interface `Lam` (an external, an annex, or a foreign declaration) mentions, which must keep its shape.
A single map cannot hold both results.

Hence, such a rewrite runs a second Rewriter with a map of its own that rebuilds only what differs and hands everything else back to the outer one:

```cpp
const Def* Scalarize::Keep::rewrite(const Def* old_def) {
    if (!has_cn(old_def) && !has_intersection(old_def)) return sca_.rewrite(old_def);
    bind_old(old_def->has_var());
    return Rewriter::rewrite(old_def); // rebuilds a Pi without flattening it
}
```

Whatever does not mention a `Cn` - or the `Var` of a binder rebuilt in here - is thus rewritten once and shared, while the nested map dies with the nested Rewriter.
Where values of the two shapes meet, the [`RWPhase`](@ref phases_coercion) coerces between them.

### Mutable Rewrites

Putting it together, a mutable is rebuilt as follows:

1. create the replacement stub,
2. map old mutable to stub immediately,
3. map binder-local substitutions - in a [`push`](@ref mim::Rewriter::push)ed scope if they must not outlive this rewrite,
4. rewrite and [`set`](@ref mim::Def::set) the operands,
5. [`pop`](@ref mim::Rewriter::pop) the scope again.

[`rewrite_stub`](@ref mim::Rewriter::rewrite_stub) implements steps 2 and 4, and finally tries to immutabilize the result in hindsight.
Custom phases mirror this pattern when they need more control - e.g. to map an old `Var` to something other than the new mutable's `Var`.
The crucial point is that recursive self-references must see the stub instead of triggering another rebuild of the same node.

If a rewrite replaces an external mutable with a fresh one, preserve the root explicitly by calling [`mim::Def::transfer_external`](@ref mim::Def::transfer_external) or re-externalizing the replacement node.

### Reshaping Aggregates

A rewrite that changes a mutable's *number* of ops - dropping components of a [`Sigma`](@ref mim::Sigma), say - cannot use the plain [`rewrite_stub`](@ref mim::Rewriter::rewrite_stub): that one rewrites `old->op(i)` into `new->op(i)` for every `i`.
The overload [`rewrite_stub(old, new, new2old)`](@ref mim::Rewriter::rewrite_stub) spells out the op indices instead: `new->op(i)` is rewritten from `old->op(new2old[i])`.
Everything else stays the same - mapping the stub before descending into it, entering it as [`curr_mut()`](@ref mim::Rewriter::curr_mut), and immutabilizing the result in hindsight unless something already captured the stub recursively.
If you fill a stub entirely by hand, [`seal`](@ref mim::Rewriter::seal) is that last step on its own.

[`Sieve`](@ref mim::Sieve) computes such an index list from a predicate over the old components:

```cpp
auto keep = mim::Sieve(old_sigma->ops(), [&](const Def* op) { return !old_world().inhabitant(op); });
if (keep.all()) return RWPhase::rewrite_mut_Sigma(old_sigma);

auto new_sigma = new_world().mut_sigma(rewrite(old_sigma->type()), keep.num_new());
return rewrite_stub(old_sigma, new_sigma, keep.new2old());
```

For an immutable aggregate, [`Sieve::gather`](@ref mim::Sieve::gather) picks the surviving ops directly.

Dropping a component shifts the indices of its successors, so every [`Extract`](@ref mim::Extract) and [`Insert`](@ref mim::Insert) into that aggregate needs its index adjusted.
This is what `keep[i]` answers - or [`Sieve::Gone`](@ref mim::Sieve::Gone), if that component did not survive:

```cpp
auto new_index = keep[mim::Lit::as(extract->index())];
```

@note Only a [`Sigma`](@ref mim::Sigma) ever needs this.
An [`Arr`](@ref mim::Arr)'s components all share one type, so it is dropped as a whole, and `World::extract_fused` splits a fused index at the `Arr` boundary - which leaves a `Sigma`'s index scalar.

#### Adding Components

A rewrite that also *adds* components - [`mem::SEO`](@ref mim::plug::mem::phase::SEO) threads a fresh φ parameter through a [`Lam`](@ref mim::Lam) for every promoted slot - [`cat`](@ref mim::Sieve::cat)s them as further candidates behind the old ones:

```cpp
auto keep = mim::Sieve(old_lam->tvars(), [&](const Def* var) { return is_kept(var); });
keep.cat(phis.size(), [&](size_t i) { return is_kept(phis[i]); });
```

They extend the very same index space, so one `Sieve` still answers the whole new layout: [`num_new`](@ref mim::Sieve::num_new) is the new arity, `keep[i]` the new index of an old component, and [`keep.cat(i)`](@ref mim::Sieve::cat) the new index of an concat'ed one.
[`all`](@ref mim::Sieve::all) then means _the new aggregate is the old one_ - nothing dropped **and** nothing appended.
Since an appended component has no old op to rewrite from, [`new2old`](@ref mim::Sieve::new2old) - and hence `rewrite_stub` - is unavailable once you append.

@warning Dropping decides *per type*, but some shapes are not the phase's to change: an [`Axm`](@ref mim::Axm)'s signature, or the dependent domain an [`App`](@ref mim::App) instantiates (`[n: Nat, «n; *»]` vs `[Nat, []]`).
Where a reshaped value meets such a shape, it must be converted back - this is what every [`RWPhase`](@ref phases_coercion) does; [`SingleErasure`](@ref mim::SingleErasure) relies on it.

### Preserving Metadata

Most rebuilds preserve debug metadata explicitly.
The default [`Rewriter`](@ref mim::Rewriter) restores the old debug info with `new_def->set(old_def->dbg_key())`, which just copies the interned index instead of re-interning the [`Dbg`](https://leissa.github.io/fe/structfe_1_1Dbg.html).
Hand-written transformations commonly propagate names or add derived names with helpers such as [`mim::Def::debug_suffix`](@ref mim::Def::debug_suffix).

If you create a replacement node that should still be recognizable in dumps, diagnostics, or generated code, preserve or adapt its debug payload immediately instead of treating naming as an afterthought.

## VarRewriter {#varrewriter}

[`VarRewriter`](@ref mim::VarRewriter) substitutes [`Var`](@ref mim::Var)s and is nothing but a [`Rewriter`](@ref mim::Rewriter) whose root [scope](@ref rewriter_scopes) owns the substituted `Var`s:
[`add(var, arg)`](@ref mim::VarRewriter::add) maps `var ↦ arg` and [`bind_old`s](@ref mim::Rewriter::bind_old) `var`.

On top of that, it only rebuilds what can change at all.
A node without free variables in any scope's `Vars` is returned as is, so unaffected subgraphs are skipped entirely.
Entering a mutable binds the mutable's own `Var`, since a binder that gets rebuilt changes its `Var`, too.

```cpp
auto result = mim::VarRewriter(var, arg).rewrite(body);

auto rw = mim::VarRewriter(world);
rw.add(var1, arg1).add(var2, arg2);
auto result = rw.rewrite(def);
```

Use [`Rewriter`](@ref mim::Rewriter) to rebuild everything, e.g. into another world; use [`VarRewriter`](@ref mim::VarRewriter) to substitute within the same world and rebuild only where necessary.

<div class="section_buttons">

| Previous                    |                  Next |
| :-------------------------- | --------------------: |
| [Developer Guide](@ref dev) | [Phases](@ref phases) |

</div>
