# mem

- SEO keeps `lam2callers_` across rounds and taints every recorded caller when a Lam's abstract vars change, so a sparse round re-joins a var from all sites; suppressing the `first_` restart instead leaves stale joins.
- SEO's cost is size × call-graph depth, not quadratic; wide, shallow programs scale fine.
- `mem.add_mem` places a memory operation with `Scheduler::late`, so sibling continuations share it.
- C interop: C `T*` parameters are element pointers `mem.Ptr0 T` (pass `mem.decay p`), and memory of unknown length is `mem.assume (p, n)`.
- `Scheduler` stays in libmim, since `ll`, `mem.add_mem`, and `mach.schedule` use it and plugins never link each other.
