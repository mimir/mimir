# Why is MimIR implemented in C++?

*A note on the cost of reimplementing MimIR in OCaml, Scala, or Haskell.*

All `sizeof`/offset figures were measured against `build-release`
(`-march=native -O3 -DNDEBUG -std=gnu++23 -DFE_ABSL`, gcc).
Code is referenced by file and symbol name rather than line number, so the
references do not rot.

This document answers a recurring question: Why not use an academically more
acclaimed language?

The short answer is that MimIR's core data structure is a mutable,
hash-consed, cyclic graph of hand-packed 72-byte nodes, and none of those
languages has an idiomatic form of that structure.

## Ballpark estimates

Slowdown and memory growth relative to the current C++ implementation, for a
faithful reimplementation of the same architecture.
Treat these as ±50%; the ordering is far more robust than the magnitudes.

| Language | Imperative subset | Idiomatic | Memory |
| -------- | ----------------- | --------- | ------ |
| OCaml (flambda) | 2.5–3.5× | 5–10× | 2.5–3.5× |
| Scala/JVM | 2.5–3.5× steady state<br>3–10× on short CLI runs | 5–12× | 3.5–5× |
| Haskell (GHC) | 3.5–6× | 8–20×, wide error bars | 3.5–5× |

The *imperative subset* column assumes mutable records, `IORef`s, and mutable
hash tables throughout — i.e. writing C++ with a garbage collector.
That concedes the entire premise of switching: you pay the full syntactic and
tooling cost and collect none of the benefit.
The *idiomatic* column is the honest comparison, and it is much worse.

## What the C++ implementation actually relies on

### `absl::flat_hash_*`

`World::unify` (`include/mim/world.h`) probes the sea of nodes on **every**
`Def` construction.
This is *the* hot path of the whole compiler.

`absl::flat_hash_set` is a SwissTable: one control byte per slot holding 7 hash
bits, probed 16-at-a-time with SSE2/NEON, payload stored inline in a flat
array.
A lookup is typically **one cache miss**.
`GIDHash` (`include/mim/util/util.h`) makes it cheaper still — the hash is
the already-computed dense `u32` gid, so hashing costs nothing.

What the alternatives offer:

- **OCaml** — `Hashtbl` is separate chaining over heap-allocated cons cells.
  Each probe is a dependent pointer chase: 2–4 cache misses instead of one, no
  SIMD, one allocation per insert.
  The idiomatic `Map`/`Set` are balanced trees: O(log n) with a chase per
  level, 5–10 misses at MimIR's node counts.
  No open-addressing table in the stdlib.
- **Haskell** — `Data.HashMap` is a HAMT: persistent, 32-way branching, and
  every insert allocates the whole path (5–7 fresh nodes).
  On a sea of nodes doing millions of inserts this becomes the dominant cost,
  not a constant factor.
  The escape (`hashtables`) drags all of `World` into `IO`/`ST`.
- **Scala** — `java.util.HashMap` and `mutable.HashMap` are chained with a
  boxed entry object per insert.
  `AnyRefMap` is the closest analogue but has no SIMD probe and no inline
  payload.

Call it **2–4× on the hash-cons probe alone**.

Also lost: `Vector = absl::InlinedVector<T, N>`
(`include/mim/util/vector.h`), which keeps small op-vectors in the object
with zero heap traffic.
None of the three has an equivalent, so every temporary `DefVec` becomes a heap
allocation.

### `Sets::Set` — a four-way sum type in one machine word

`Sets::Set` (`submodules/fe/include/fe/sets.h`) is a single `uintptr_t` with two tag
bits:

    Null | Uniq (D* inline) | Data (arena FAM) | Node (trie node)

Consequences:

- `Def::vars_` and `Def::muts_` cost **8 bytes each**.
- A singleton set *is* the pointer to its element — zero allocation, zero
  indirection, and `contains` is one comparison.
- Set equality is `ptr_ == ptr_` over the entire set, because everything is
  hash-consed.

In OCaml a 4-constructor variant with payloads is 3 boxed blocks plus 1
immediate; in Haskell the same plus a thunk per constructor; on the JVM four
classes and a megamorphic call site.
Pointer tagging is reachable only via `Obj.magic` or `Unsafe`, at which point
you have left the language.

`Sets::Data` is a **C99 flexible array member**
(`size_t size; D* elems[];`) placement-`new`ed into `data_arena_` — not even
portable C++.
Elsewhere it becomes a record plus a separate array object: two allocations, an
extra hop per element access, and on the JVM a second 16-byte header.

### The link-cut tree mutates in place

`lct::Node` (`submodules/fe/include/fe/link_cut_tree.h`) is CRTP-intrusive, embedded
directly into the trie `Node` by inheritance.
`rotate` is six pointer stores into memory that already exists.
`splay`, `expose`, `lca`, `is_descendant_of` are pure in-place pointer surgery
with **zero allocation**.

This is where a functional formulation loses *asymptotically* rather than by a
constant.
A splay tree is amortized O(log n) **because it mutates on read**:
`Set::contains` → `LCT::find` → `expose` → `splay`, so a membership test
restructures the tree.
A persistent splay tree allocates O(depth) nodes **per query**, and
`Sets::Set::has_intersection` calls `find` inside a loop over two
node-sets.

### Arenas, and how `Def`s are placed in them

`sizeof(Def)` is **72 bytes** — measured against `build-release`
(`-march=native -O3 -DNDEBUG -std=gnu++23 -DFE_ABSL`); 80 in a Debug build,
where `curr_op_` is present.
The layout has **zero padding**:

| off | field | bytes |
| --- | ----- | ----- |
| 0  | `normalizer_` / `axm_` / `var_` / `binder_` / `world_` (union) | 8 |
| 8  | `flags_` | 8 |
| 16 | `curry_`, `trip_` | 1 + 1 |
| 18 | `node_` | 1 |
| 19 | `mut_:1  external_:1  annex_:1  dirty_:1  dep_:4` | 1 |
| 20 | `mark_` | 4 |
| 24 | `gid_` | 4 |
| 28 | `num_ops_` | 4 |
| 32 | `hash_` | 8 |
| 40 | `vars_` | 8 |
| 48 | `muts_` | 8 |
| 56 | `dbg_` | 4 |
| 60 | `tid_` | 4 |
| 64 | `type_` | 8 |
| | **total** | **72** |

Three deliberate optimizations produce that number, and none of them survives a
port:

- **`dbg_` is a `u32` index** into the Driver's `Dbg` table rather than an
  inline `Dbg` (see `Def`'s data members in `include/mim/def.h`).
  That alone is 20 bytes off *every* node, and `Dbg`s are shared roughly 10:1
  in practice.
  It is deliberately placed adjacent to `tid_` so the two `u32`s share one
  8-byte slot.
- **`Def` is not polymorphic.**
  There is no vtable pointer — verified: `std::is_polymorphic_v<Def>` is
  `false`.
  `immutabilize` and `arity` are ordinary member functions that `switch` on the
  1-byte `node_` tag (`src/mim/def.cpp`), and a subclass
  accidentally growing a `virtual` is caught by the `sizeof(Def) == sizeof(T)`
  assert in `World::allocate`.
- **Sub-word packing.**
  `u32` gids rather than `size_t`, a five-way union, and five flags sharing the
  single byte left over by the `u8` `node_` tag.

`World::allocate` (`include/mim/world.h`):

```cpp
static_assert(sizeof(Def) == sizeof(T),
              "you are not allowed to introduce any additional data in subclasses of Def");
auto num_bytes = sizeof(Def) + sizeof(uintptr_t) * num_ops;
auto ptr       = move_.arena.defs.allocate(num_bytes, alignof(T));
auto res       = new (ptr) T(std::forward<Args>(args)...);
```

So every subclass is layout-identical — `App`, `Lam`, `Sigma` add methods,
never data — and one bump-pointer allocation covers header *and* operands.
`op(i)` indexes off `this + 1`: no second object, no header, no indirection.

In OCaml every field is a tagged word and bitfields do not exist; on the JVM
sub-word fields cannot be packed at all, and the object header alone is 12–16
bytes before a single field.
A faithful port lands around 120–140 bytes per node *plus* a separate operand
array with its own header — roughly 2–2.5× on the node before any allocator or
GC effect.

**One place where the gap narrows, in fairness.**
Dispatching by `switch` on a 1-byte tag is precisely what an ML pattern match
compiles to.
By dropping virtual dispatch, MimIR has converged on the functional languages'
dispatch strategy rather than C++'s, so this particular optimization is roughly
neutral in a port — OCaml and Haskell would get it for free and more legibly.
The saving is the *vptr*, not the dispatch.

### Speculative construction with arena rollback

This one has no counterpart anywhere — `World::unify`:

```cpp
auto state = move_.arena.defs.state();
auto def   = allocate<T>(num_ops, ...);          // speculatively construct
if (auto [i, ins] = move_.defs.emplace(def); !ins) {
    deallocate<T>(state, def);                    // ~T(), rewind bump ptr, --curr_gid
    return static_cast<const T*>(*i);
}
```

The node is constructed speculatively — normalizer run, hash computed, free
vars computed — then probed against the sea, and on a hit it is *un-allocated*
and the gid counter rolled back.
In a normalizing hash-consed IR the hit rate is high by construction, so the
common path costs **zero net allocation and produces zero garbage**.

`Sets::unify` does the same for `Data`.
`Sets::merge` goes further: it allocates the upper bound
`d1->size + d2->size`, merges into it, then returns the unused tail via
`unify(data, state, excess)`.
Shrinking your most recent allocation is a bump-pointer-only move.

In fairness, a generational nursery handles short-lived garbage well — copying
cost is proportional to survivors, so the dead speculative `Def` is nearly free
to collect.
The cost is the **compounding**: high nursery churn forces frequent minor
collections, and each must scan a remembered set that is enormous here, because
MimIR constantly mutates old-generation `Def`s — `set()`, the users set in
`muts_`, `mark_` sweeps in `free_vars()`, and lazy `tid_` assignment in
`Sets::set_tid`, which writes into a nominally-immutable `Def`
from inside the trie.
Every one of those is an old→young pointer write paying `caml_modify` or
card-marking.
High churn × large remembered set is the bad quadrant.

`RWPhase`'s world-swap is the same trick at macro scale: drop the whole old
`World` and its arenas in O(1), with no tracing.
A GC has to *prove* the old world is dead by walking it.

## Why idiomatic style is much worse

### Pointers become map lookups

An immutable graph cannot have back-edges, and MimIR's IR is cyclic by
construction — mutables reference themselves for recursion, and `muts_` doubles
as the users set.
Idiomatic functional style has two answers, both bad here:

- *Knot-tying* (`let rec` / laziness) builds the cycle but leaves sharing
  unobservable: no `gid`, no hash-consing, no users set.
  Fatal for a sea of nodes.
- *Explicit ids into a store* (`IntMap Node`) is what real functional compilers
  do.
  Now `def->op(0)` is a Patricia-trie lookup instead of a pointer dereference:
  ~5–10 dependent loads replacing 1, on the most executed operation in the
  compiler.
  Structural, not tunable.

And since the store is persistent, every rewrite path-copies it.
`RWPhase` currently drops an arena in O(1); idiomatically it allocates a new
HAMT spine per touched node.

### Hash-consing is inherently impure

The sea of nodes *is* a mutable global table.
The pure alternatives are a `State World` monad threaded through every
constructor — so `Def` construction returns a new world and the HAMT insert
cost lands on every node — or `unsafePerformIO` over a global `IORef`, which is
what actual Haskell hash-consing does and is not idiomatic.

### Caches must move out of the node

`vars_`, `muts_`, `mark_`, `dirty_`, and `tid_` are mutable memo fields on
nominally-immutable `Def`s.
Idiomatically they become separate memo maps keyed by id — another lookup per
access — and the invalidation currently free from `dirty_` has to be threaded
explicitly.

### Haskell specifically

Laziness and hash-consing are directly opposed: hash-consing requires forcing
at construction, so `Def` ends up `!`-annotated throughout.
You would be writing strict Haskell with lazy Haskell's tooling and space-leak
failure modes.

## The empirical argument

Every serious compiler written in these languages **breaks idiom at exactly the
point where MimIR is most demanding**:

- **GHC** is written in Haskell, and Core is a **tree**, not a hash-consed DAG.
  No global uniqueness table; `Unique`s come from `UniqSupply`, a splitting
  counter that is famously impure and famously a wart.
  GHC did not choose a tree IR because trees are better — it chose one because
  a mutable, hash-consed, back-edged graph is not something you write in
  Haskell.
- **Coq's kernel** does hash-cons — with imperative `Hashtbl` and mutable
  state, in OCaml.
- **The OCaml compiler** uses mutable `Ident` stamps from a global counter.
- **scalac** has a pervasively mutable `Symbol` table, and is still slow.

The pattern is a revealed preference: as soon as a real compiler in these
languages needs what MimIR needs, it either abandons idiom or abandons the
design.

## The idiomatic dividend does not apply here

The ADT-plus-exhaustive-matching payoff is largest for a **tree-shaped,
closed-ADT, non-hash-consed** IR.
MimIR is a **graph-shaped, open-node-set, hash-consed** IR whose node set is
extensible at runtime by `dlopen`'d plugins, with terms and types in one graph
because dependent types make a closed ADT impossible in the first place.
`match d with App (f, a) -> ...` buys almost nothing over
`if (auto app = d->isa<App>())` when the node set is open anyway.
Those are nearly opposite design points: you would pay the full idiomatic tax
on a design that collects almost none of the idiomatic dividend.

## Non-performance costs

1. **Plugins.**
   The architecture is `dlopen`'d modules exporting `mim_get_plugin`, with
   normalizers stored as raw function pointers *inside the `Def` union*.
   JVM classloaders do this well.
   OCaml's `Dynlink` works but is brittle across compiler versions and awkward
   under flambda.
   Haskell dynamic loading is painful (ABI tied to exact package hashes); you
   would realistically abandon `dlopen` for static-link-all-plugins, killing
   the auto-discovered `extra/*` third-party story.
2. **LLVM.**
   Neutral today since the `ll` backend emits text, but the C++ API is out of
   reach behind perpetually-stale FFI bindings in all three.

## Two honest caveats

**MimIR's current performance problems are not constant-factor problems.**
`RWPhase` rebuilds the entire world per phase, `PhaseMan` iterates to fixpoint,
free vars are re-derived via `mark_` sweeps.
Those are algorithmic, and a 2× language penalty is noise beside traversing the
world N times.
This is *not* an argument that the language does not matter — the penalty is
multiplicative on top of the algorithmic cost, not an alternative explanation
for it.
The sharper point is that a tracing GC **removes entire classes of fix from the
toolbox**: when you profile and decide to stop allocating, the fixes you reach
for are rollback-on-hit, in-place mutation, and arena-scoped scratch — exactly
the three things OCaml, Haskell, and Scala cannot express.

**At the idiomatic end the question is arguably ill-posed.**
Nobody would write *MimIR's* IR that way; they would write a different IR —
tree-structured Core with a `Map`-based store — and then the comparison is
about IR design, not language throughput.
A persistent world does buy real things: free structural sharing across phases,
trivially correct speculative rewriting, `RWPhase` as a no-op.
MimIR obtains those from arena-drop and world-swap instead, at a fraction of
the cost.

## The escape hatch, and why it defeats the purpose

Everything above is recoverable: `Bigarray` plus `Obj.magic` in OCaml,
`MutableByteArray#` plus manual offset arithmetic in `IO` for Haskell,
`sun.misc.Unsafe` or Panama `MemorySegment` on the JVM.
That lands somewhere around **1.3–1.5×**.

But look at what you would be writing: manual offset arithmetic into a byte
array, hand-rolled pointer tagging, an unsafe-cast class hierarchy, a mutable
splay tree in `IO`.
That is strictly worse ergonomics than the current C++, and you have given up
ADTs, exhaustive matching, and type safety — the entire reason to switch.

MimIR's core is not idiomatic C++ either.
It is written in the systems-programming subset: flexible array members,
placement new, tag bits, intrusive CRTP, arena rollback, hand-packed layout.
Those languages do not have a worse version of that subset; they do not have it
at all.

## What about Rust?

Rust is the only language on the list that offers the same systems-programming
subset, so it deserves a separate answer.

**The honest reason is historic.**
MimIR descends from Thorin, which was C++ from the outset, years before Rust
1.0 was a plausible choice for a compiler framework — and the surrounding
ecosystem (LLVM, and the C++ literacy of everyone in a compilers group) pointed
the same way.
That is a perfectly good reason for how we got here.
The interesting question is whether the technical case would justify moving,
and it does not: the gain is close to zero and the losses are concrete.

### Rust would land at roughly performance parity

Estimate: **1.0–1.3×**, i.e. within noise of the C++ — but only via a design
that gives up most of what makes the current implementation tight.

A mutable, cyclic, aliased graph is the one area Rust is *known* to be
awkward at, and the standard advice for graphs in Rust is exactly the
workaround: **stop using pointers and use arena indices**.
That advice is not folklore; it is what Rust compilers actually do.
`rustc` interns types into arenas and threads `&'tcx` references and
`rustc_index::IndexVec` indices everywhere.
Cranelift indexes everything through `cranelift-entity`.
`egg` hash-conses e-nodes behind `u32` ids and a union-find.
None of them builds a pointer-linked mutable graph.

Index-based arena Rust is a legitimate design and would perform fine — an
index is a bounds-checked load, not a hash lookup, so unlike the OCaml/Haskell
case there is no asymptotic loss.
But it is a *different* implementation, and each of MimIR's five load-bearing
structures pays something:

- **No flexible array members.**
  `sizeof(Def) + 8 * num_ops` in a single bump allocation with
  `ops_ptr() == this + 1` has no safe Rust equivalent; custom DSTs are still
  unstable.
  Either `ops: &'a [&'a Def]` — a 16-byte fat pointer replacing the inline
  `u32 num_ops_`, plus a second allocation and an extra indirection — or manual
  `Layout` arithmetic in `unsafe`.
  `rustc` chose the latter: `rustc_middle::ty::List<T>` is a hand-rolled
  header-plus-trailing-elements allocation written in `unsafe`, precisely
  because the language does not provide one.
- **No tagged pointers.**
  `Sets::Set` packs a four-way sum into one `uintptr_t` with two tag bits.
  A Rust `enum` over four pointer-carrying variants is 16 bytes; niche
  optimization does not apply.
  That is `vars_` and `muts_` going 8 → 16 bytes each, +16 per node, unless you
  hand-roll it with `NonNull` and `unsafe`.
- **The rollback trick is the most anti-Rust pattern in the codebase.**
  Rewinding a bump allocator past objects whose references may have escaped is
  exactly what the borrow checker exists to prevent, and `bumpalo` offers no
  checkpoint/rewind in its safe API — only a full `reset()`.
  The C++ already guards this by comment and assertion (`create_trie` must draw
  only from `node_arena_`); Rust would want
  that invariant in the type system and cannot express it, so it becomes
  `unsafe` with a `'a` lifetime that is a lie.
- **The intrusive link-cut tree** is the textbook "you will write this with
  indices or `unsafe`" structure: parent pointers plus `rotate` re-parenting
  arbitrarily.
  Worse, `splay` mutates during `contains` — a logically-`&self` operation that
  restructures the tree.
  In Rust that is `&mut self` (viral, and unusable while other borrows into the
  same arena are live) or interior mutability over indices.
- **`mutable` fields are fine, in fairness.**
  `Cell<u32>` for `dbg_`, `tid_`, `mark_` and `Cell<Option<&'a Def>>` for
  `type_` are genuinely zero-cost.
  This one is syntactic noise, not a performance cost, and it would be
  churlish to claim otherwise.

The `Rc<RefCell<Def>>` design that a newcomer would reach for first is the one
option that is clearly worse: refcount cycles leak by construction — and in a
sea of nodes with `muts_` holding back-edges there is no spanning tree to make
`Weak`, so there is no principled place to break them — plus a runtime borrow
check on every access, whose failure mode is a panic rather than a compile
error.

### Where Rust genuinely wins

Not nothing, and worth stating plainly:

- **Cargo instead of CMake.** A real, daily quality-of-life improvement.
- **Thread safety.** MimIR is single-threaded today.
  If parallelising phases ever becomes a goal, `Send`/`Sync` is a far better
  foundation than anything C++ offers, and this is the one argument that could
  actually change the verdict later.
- **Miri**, which would check exactly the `unsafe` core that a port would
  produce.
- **`enum` plus exhaustive `match`** for the genuinely closed parts, and
  tooling (rust-analyzer, built-in test harness) that is simply better.

### The plugin story is worse, not better

This is the decisive practical point, because plugins are core architecture,
not a peripheral feature.

MimIR loads `dlopen`'d modules exporting `mim_get_plugin`, which register
normalizers as raw function pointers stored inside the `Def` union.
**Rust has no stable ABI.**
Every plugin would have to be compiled with the exact same rustc version as
`libmim`, or the entire interface reduced to `extern "C"` shims with
`#[repr(C)]` types on both sides — losing the very type safety that motivated
the move.
C++ already has friction here (`absl` containers must not appear in types that
cross the `dlopen` boundary), but "keep `absl` out of the interface" is a much
smaller constraint than "no stable ABI exists".

### Verdict

Roughly performance parity, in exchange for an `unsafe` core that reimplements
the flexible array member and the tagged pointer by hand, a rollback pattern
the borrow checker is specifically designed to reject, and a materially worse
plugin ABI story — against real wins in build tooling, future parallelism, and
`unsafe`-checking.

For a *new* project with these requirements the choice would be genuinely
close, and the thread-safety argument might well decide it.
For an existing, working, tuned implementation, that is not a trade that pays
for itself.

## Summary

> MimIR's IR is a mutable, hash-consed, cyclic graph of hand-packed 72-byte
> nodes with zero padding and no vtable pointer.
> That is not a data structure those languages have an idiomatic form of — and
> every compiler written in them that needed one wrote imperative code to get
> it.

Rust is the only entry on the list that offers the same subset, and there the
answer is parity rather than a win — see *What about Rust?* above.
The reason MimIR is in C++ is historic; the reason it stays there is that
nothing on offer would pay for the move.
