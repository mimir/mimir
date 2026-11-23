# mem

- SEO keeps `lam2callers_` across rounds and taints every recorded caller when a Lam's abstract vars change, so a sparse round re-joins a var from all sites; suppressing the `first_` restart instead leaves stale joins.
- SEO's `finalize` pins an unknown Lam's whole var besides its projections, because `apply_known` writes `var ↦ tuple` too.
- SEO's cost is size × call-graph depth, not quadratic; wide, shallow programs scale fine.
- `mem.add_mem` places a memory operation with `Scheduler::late`, so sibling continuations share it.
- C interop: C `T*` parameters are element pointers `mem.Ptr0 T` (pass `mem.decay p`), and memory of unknown length is `mem.assume (p, n)`.
- `mem.sched`, `mem.legalize_native`, and `mem.schedule` serve the native backends (`x86`, `arm`); the phases take the machine from `Driver::target` and run only under `compile.cond` in `opt.mim`.
  `Scheduler` stays in libmim, since `ll` and `mem.add_mem` use it too and plugins never link each other.
