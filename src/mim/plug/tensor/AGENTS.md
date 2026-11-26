# tensor

- `lower_via_impl` re-applies an axiom's groups positionally to its `_impl` Lam, so an axiom and its `_impl` must keep identical curry shapes.
- `buffer.Buf` normalizes literal size-1 axes out of its shape, so an implicit that must hold a *logical* shape cannot be inferred from a `Buf` operand; `matrix.concat` and `matrix.map_reduce_aff` therefore stay manually curried in `lower_to_mem`.
- Fusion is guarded by `reads_as_produced`: size-1 axes collapse in types, so a slot may declare another shape than produced.
- The tensor lit tests take 5–12 s each; a "fast" run usually means the compile errored out.

## `reassoc`

- Matrix-chain DP over `tensor.product_2d` with costs as polynomials in the symbolic extents, Pareto-pruned, with a runtime `core.ncmp` dispatcher when several bracketings survive.
- Two `lower_to_mem` gaps block the dynamic path, neither caused by `reassoc`: a dynamically shaped tensor cannot cross a function boundary, and a tensor-typed function *value* is rejected.
  Check any design that makes a tensor cross a function boundary against both.
- A BLAS `gemm` hook belongs at `btensor`, after bufferization, matching on the ring (f32/f64 only), never at the `tensor` level.
- Next direction: generalize to `tensor.dot_product` networks via subset DP with a Pareto frontier per DP cell; with pairwise-distinct symbolic extents nothing prunes, so `Catalan(n−1)` enumeration does not scale.
- The dumper does not print a `lam` reachable only as a tuple element under an `Extract`, so dispatcher thunks look absent in `-o` output.
