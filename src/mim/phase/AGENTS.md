# Phases

- Scalarize is type-directed: it flattens per immutable `Cn` Pi, not per Lam, so every producer and consumer of a type is reshaped in one sweep.
  Interface signatures go through the nested `Scalarize::Keep` rewriter; a mutable with a Cn (a dependent Pi like `{m n: Nat} → …`) must be rebuilt there, or curried externs flatten.
  Only two keeps remain: `keep_opaque` is needed for correctness (a `Cn` behind a pointer or a Variant, which coerce() cannot take apart), and a dynamically indexed param is kept for code quality only.
  An interface's other params are spread and reassembled where used whole (`mem.store`, `core.bitcast`).
- EtaConv gives every occurrence of an unknown Lam its own fresh `eta_` wrapper, like a per-call-site join point; the missing `map` on those paths is intended.
- EtaConv unfolds an immutable that contains a mutable (other than an App or a Var) into a tree up to the next App or mutable: it is analyzed and rewritten once per occurrence, so a hash-consed branch tuple shared by several Apps gets wrappers per App and no critical edge arises.
  A Lam's body App counts as its own occurrence too, so Lams with the same body (`lit/eta_shared_body.mim`) get separate wrappers; analysis and rewrite must agree on this, or the `compile.phases tt` loop never converges.
- After EtaConv, every Lam is either *unknown* (used as a value) or *well-known* (all uses are calls or jumps); use these words, not "escaping".
- StaticArgOpt seeds with the rightmost Pi-typed domain, freezes the sites forwarding it, and pools every other domain static across them; per-site routing is mandatory (sites outside the class call the wrapper).
- A phase scanning the World is written like `beta_red.cpp`: an `analyze(const Def*)` member guarded by a `DefSet`, recursing into `deps()`; use `Nest` only when you need scope, dominance, or SCCs.
- A phase that post-processes a Lam's filter or body overrides `RWPhase::rewrite_ops`, not `rewrite_mut_Lam`; `DS2CPS` (lifting direct-style calls of CPS functions into continuations) is shared by `cps.conv`, `mach.legalize`, and `tensor.lower_to_mem`.
- Verify behavior-preserving changes with the optimizer dump oracle (see the root `AGENTS.md`).
