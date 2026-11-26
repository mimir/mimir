# Plugins

## Axiom design

- Never put a computed size (`core.nat.add (a, b)`) in a *domain*: unification is purely structural and cannot invert it.
  Computing forward in the codomain is fine; reparametrize (e.g. by the total) and recover the parts in the lowering.
- A literal-rank `«s; T»` only reverse-unifies when an earlier operand pins the rank, since a Sigma domain is checked in declaration order.
  Swap operands when the pinning operand determines the later operand's values; curry when it only determines its type.
- A literal-1 extent collapses out of the type (`«1, 3; I32»` is `«3; I32»`), so a shape with size-1 axes cannot be inferred back.
- Put inference-friendly meta into a carrier type (`buffer.Buf (r, s, T)`), not into an array shape.
- libmim core knows no plugin axioms: no `#include` of `plug/` from `src/mim/` or `include/mim/`.

## Conventions

- `mem.M`/`mem.Ptr` are aliases of `core.M`/`core.Ptr`; spell them `mem.*` everywhere except where `mem` must not be a dependency (dumps print `core.*`).
- A top-level cross-plugin `anx X = other.Y;` is a foreign alias: bootstrap emits `using X = plug::other::Y;`.
- `mem.Char` is for semantic characters only; raw buffers and handles stay `I8`.
- Plugin-local free functions (e.g. `reg_phases`) must be `static`: `MIM_STATIC_PLUGINS` links all plugins into one binary.
- Cross-plugin symbol lookup goes through `Plugin::syms` (`PluginSym`), not `dlsym`.
- Layout decisions (sizes, niches) are the backend's call; keep them symbolic in the IR until late.
