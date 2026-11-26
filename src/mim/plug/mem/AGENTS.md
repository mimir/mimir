# mem

- SEO keeps `lam2callers_` across rounds and taints every recorded caller when a Lam's abstract vars change, so a sparse round re-joins a var from all sites; suppressing the `first_` restart instead leaves stale joins.
- SEO's cost is size × call-graph depth, not quadratic; wide, shallow programs scale fine.
- `mem.add_mem` places a memory operation with `Scheduler::late`, so sibling continuations share it.
- Gap: `mem.add_mem` projects the Var of a bodyless Lam with a dependent domain (btensor's `extern con print_int_matrix [mm: mem.M 0, k l: Nat, m: buffer.Buf (2, (k, l), I32), …]`) before its rebuilt domain is set, which fails with `a tuple element projects out of a mutable that is not set yet`; the `lit/btensor/print_*` tests and `test_write.mim` hit it under `-p x86`.
- `core::mem_def` only looks at a value and its direct projections: a memory is never nested deeper, and descending further would unfold a recursive type forever.
- C interop: C `T*` parameters are element pointers `mem.Ptr0 T` (pass `mem.decay p`), and memory of unknown length is `mem.assume (p, n)`.
- `Scheduler` stays in libmim, since `ll`, `mem.add_mem`, and `mach.schedule` use it and plugins never link each other.
