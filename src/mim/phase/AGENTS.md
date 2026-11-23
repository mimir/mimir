# Phases

- Scalarize is type-directed: it flattens per immutable `Cn` Pi, not per Lam, so every producer and consumer of a type is reshaped in one sweep.
  Interface signatures go through the nested `Scalarize::Keep` rewriter; a mutable with a Cn (a dependent Pi like `{m n: Nat} → …`) must be rebuilt there, or curried externs flatten.
  The remaining keeps (dynamically indexed params, `tensor.buf`, `pin_opaque`) each guard a plugin's shape assumption.
- EtaConv gives every occurrence of an unknown Lam its own fresh `eta_` wrapper, like a per-call-site join point; the missing `map` on those paths is intended.
- EtaConv does not rule out critical edges: a hash-consed branch tuple used by several branches shares its wrappers.
  A phase that assumes no critical edges must split them itself, as `x86.spilling` does.
- After EtaConv, every Lam is either *unknown* (used as a value) or *well-known* (all uses are calls or jumps); use these words, not "escaping".
- StaticArgOpt seeds with the rightmost Pi-typed domain, freezes the sites forwarding it, and pools every other domain static across them; per-site routing is mandatory (sites outside the class call the wrapper).
- A phase scanning the World is written like `beta_red.cpp`: an `analyze(const Def*)` member guarded by a `DefSet`, recursing into `deps()`; use `Nest` only when you need scope, dominance, or SCCs.
- Verify behavior-preserving changes with the optimizer dump oracle (see the root `AGENTS.md`).
