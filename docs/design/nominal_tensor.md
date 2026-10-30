# Nominal, shape-indexed tensors

`Tensor r s T` distinguishes tensor data from shape vectors, index vectors and ordinary tuples. Structural
arrays alone cannot express that distinction: all of them can have the same `Arr` type, and singleton or
empty axes can erase shape or element-type information.

## Representation

```mim
priv nom Tensor (r: Nat) (s: «r; Nat») (T: *) = [indices: «(r, s, ⊥:T)», data: «s; T»];
pub anx let Tensor = Tensor;
```

All applications share one nominal declaration key. The singleton witness retains the complete `(r, s, T)`
contract even when the array representation folds away. Thus `Tensor 1 0 I32` differs from `Tensor 1 0 I64`,
`Tensor 2 (1, 4) I32` differs from `Tensor 1 4 I32`, and `Tensor 0 () I32` differs from `I32`.

The typed bottom `⊥:T` is a type marker inside the singleton, never an evaluated data element. It records `T`
without raising the universe of `Tensor r s T` above `*`. Singleton erasure removes the witness before codegen.

`priv nom` hides representation construction and elimination from clients. `pub` exports the abstract type
constructor; `anx` registers it so C++ conversion can identify its nominal key.

## Boundary operations

```mim
axm from_array: {r: Nat, s: «r; Nat», T: *} → [a: «s; T»] → Tensor r s T;
axm get: {r: Nat, s: «r; Nat», T: *}
       → [t: Tensor r s T, i: «j: r; Idx (s#j)»] → T, normalize_get;
axm set: {r: Nat, s: «r; Nat», T: *}
       → [t: Tensor r s T, i: «j: r; Idx (s#j)», v: T] → Tensor r s T, normalize_set;
axm scalar: {T: *} → T → Tensor 0 () T, normalize_scalar;
axm item: {T: *} → Tensor 0 () T → T, normalize_item;
```

These operations remain axioms until conversion. Ordinary lambda implementations, including accessors
annotated `@ff`, can inline before bufferization and expose the underlying array representation.

`get` and `set` take one coordinate per logical axis. Shape `(2, 3)` requires `[Idx 2, Idx 3]`; rank 0 requires
`()`. This dependent index contract does not insert runtime bounds checks. In MimIR, `Idx 0` denotes 2⁶⁴ values,
so zero extents also require appropriate iteration-domain handling.

`from_array` is reserved for static shapes and literal arrays. Uniform literals use `buffer.lit`; other
literals use `buffer.init`. Validation happens during conversion, allowing generic definitions and unresolved
inference to exist before specialization. External data enters through Tensor-typed parameters. There is no
public `to_array` operation or dynamic Pack materializer.

Array folding cannot recover logical singleton axes at ingress. Supply `from_array @(r, s, T) a` when the
payload alone cannot determine the intended shape. `scalar` and `item` provide explicit rank-0 conversions.

Shape queries read type indices, so `shape` remains an ordinary lambda. Shapes, schedules, permutations and
access maps retain their existing array types; only tensor-valued operands and results use `Tensor`.

## Normalization

Normalizers fold known compositions without exposing a residual Tensor representation:

- `get (from_array a, i)` uses array extraction.
- `set (from_array a, i, v)` folds literal updates using array insertion and returns another `from_array`.
- `get (set (t, i, v), i)` returns `v`; provably distinct literal coordinates bypass the write.
- Reads through broadcast and repeat map coordinates back to the input when that mapping is known.
- `item (scalar x)` returns `x`; compatible scalar/item round trips cancel.

Operations that cannot fold remain axiom applications. Ordinary nominal values unrelated to Tensor are not
restricted by these rules.

## Bufferization

`LowerToMem::mentions_tensor` recognizes the Tensor nominal key and searches aggregate and callable types
for that key. It no longer guesses tensor roles from an array type's use by an operation. `is_tensor_fn`
classifies the function's signature; conversion eligibility is checked separately.

The type rewrite is `Tensor r s T` → `buffer.Buf (r, s, T)`. Ordinary Rewriter traversal propagates that change
through function types, aggregates, parameters and calls. Parameter groups and curry structure are preserved;
this conversion introduces no parameter flattening, uncurrying or new calling convention.

| Tensor operation | Buffer operation |
|---|---|
| `from_array a` | `buffer.lit` or `buffer.init` |
| `scalar x` | Rank-0 `buffer.init` |
| `get (t, i)` / `item t` | `buffer.read` |
| `set (t, i, v)` | Allocate a copy, `buffer.copy`, then `buffer.write` |
| `generate` / `splat` | Allocate and fill |
| Map-reduce, broadcast, pad, concat, gather, scatter | Existing destination-passing btensor operations |

Conversion reads logical `(r, s, T)` from the nominal witness. Buffer types may fold singleton axes, so buffer
indices fold with the same logical shape. This keeps logical shape metadata separate from physical buffer types.

Existing fresh-memory continuation bookkeeping remains in place. `mem.add_mem` subsequently threads the
memory operands through the CPS program. Local continuations and grouped parameters use the same type rewrite
as external defined functions.

Residual direct-style tensor computation requires prior inlining or memory-aware CPS conversion. Unset foreign
Tensor functions, unsupported higher-order contracts and foreign aggregate carriers are diagnosed rather than
given a new ABI. A residual Tensor `Name` or `Struc` indicates that the private representation escaped.

## Scope

Nominal typing preserves symbolic extents as type indices. Runtime extent erasure, runtime-sized copy loops,
shape-family ABI conversion and dynamic-model execution are separate buffer-lowering work. The nominal type
change does not introduce a tensor descriptor ABI or runtime rank support.

Literal zero extents are currently rejected by both bufferization and value lowering. Nominal type identity
still preserves the indices of empty tensors; implementing their operations is separate work. Empty-domain
handling in btensor and diagnostics for direct-style memory effects are independent fixes.

The frontend must construct `Tensor r s T` through the annex, use Tensor-typed external parameters, and use
`get`/`item` for element access. The `torch` dialect remains a separate migration.
