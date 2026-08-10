#pragma once

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Bufferizes the low-level tensor axioms onto the shared `buffer` layer.
/// `get` / `set` become `%buffer.read` / `%buffer.write`, `map_reduce` / `broadcast` / `pad` / `concat`
/// become their buffer-world `%matrix.*` counterparts,
/// and tensor array values `«s; T»` become `%buffer.Buf (r, s, T)` handles.
/// Afterwards `%buffer.lower_ptr` lowers the buffer layer to `%mem.Ptr` + `%mem.lea` / `%mem.load` / `%mem.store`.
///
/// This phase is *conversion-only*: it rewrites types and operations but does not thread the `%mem.M`
/// memory monad itself. Emitted buffer operations consume a `⊥: %mem.M 0` placeholder (or a short local
/// chain rooted in a LowerToMem::fresh_mem continuation's var), and the SSA value dependencies keep them
/// anchored and ordered. The `%mem.add_mem` phase
/// (mim::plug::mem::phase::AddMem), scheduled right after this one in the pipeline, then mem-extends all
/// continuations and rewires every memory operand to the scheduler-placed current memory — handling returns,
/// error continuations, join points, branch arms, and interleaving with a caller's own memory operations
/// uniformly.
///
/// Which array types denote tensors (as opposed to index/shape arrays that share the `Arr` structure) is
/// determined by *role*: a pre-pass collects the array operand/result types of the tensor operations, and only
/// those types are rewritten to `Buf` — and only at function boundaries, never as a global type rewrite.
class LowerToMem : public RWPhase {
public:
    LowerToMem(World& world, flags_t annex)
        : RWPhase(world, annex) {}

private:
    void start() override;
    const Def* rewrite(const Def*) override;
    const Def* rewrite_mut_Lam(Lam*) override;
    const Def* rewrite_imm_App(const App*) override;

    /// The conversion part of LowerToMem::rewrite_mut_Lam (boundary conversion, local continuations, or the
    /// generic RWPhase rewrite); the override itself only scopes the fresh-memory bookkeeping around it.
    const Def* conv_mut_Lam(Lam*);

    const Def* lower_get(const App*);
    const Def* lower_set(const App*);
    const Def* lower_broadcast(const App*);
    const Def* lower_map_reduce(const App*);
    const Def* lower_pad(const App*);
    const Def* lower_concat(const App*);

    /// Adapts a call to a bufferized function: materializes value-world tensor arguments into buffers;
    /// continuation arguments pass through (their domains are converted by `rewrite_mut_Lam`).
    const Def* lower_call(const App*, Lam* old_callee);

    /// Converts an argument to a converted parameter type: tensor values become buffers (via `%buffer.init`
    /// on a `⊥` memory), recursing through sigmas; anything else is `rewrite`d.
    const Def* materialize(const Def* old_ty, const Def* old_arg);

    /// Fills a fresh buffer of (old) array type `arr_ty` with the (already rewritten) scalar `scalar`, via
    /// `%buffer.constant`. `%matrix.lower_aff` turns that into a fill loop, so it never materializes as a
    /// monolithic `%mem.store` of a giant literal array (which the LLVM backend cannot digest).
    /// Used for constant splats `‹s; c›` and scalar `%tensor.broadcast`s.
    const Def* splat_buffer(const Def* arr_ty, const Def* scalar);

    /// Buffer-reuse policy. `false` (the initial *always-allocate-and-copy* policy) is always sound.
    /// A future liveness-based policy may return `true` to write into the source buffer in place.
    bool reuse_in_place(const App*) const { return false; }

    /// Pre-pass: records every array type used as a tensor operand/result and the set of bufferized
    /// functions; hard-errors on program shapes the conversion cannot handle (there is no value-semantics
    /// fallback in the default pipeline).
    void collect_tensor_types();

    /// Builds the `%buffer.Buf` type for an old tensor array type `«s; T»` (peeling the nested `Arr`s).
    const Def* buf_of(const Def* arr_ty);

    /// Converts a boundary type: tensor array types become `%buffer.Buf`, recursing through (immutable) sigmas
    /// so that grouped parameters like `[«s; T», Idx s]` are converted as well; anything else is `rewrite`d.
    const Def* conv_boundary(const Def* t);

    /// Drops, from the (unfolded) index `idx`, the components of size-1 dimensions of `shape`.
    /// MimIR folds size-1 dimensions out of array/buffer types (`«3,1;T»` ≡ `«3;T»`), so an index addressing a
    /// buffer must match the folded shape.
    const Def* fold_index(const Def* shape, const Def* idx);

    /// A `⊥: %mem.M 0` placeholder consumed by emitted buffer operations; AddMem replaces it with the
    /// scheduler-placed current memory.
    /// Shared by every op whose result is a pure function of its value operands, so that genuinely equal
    /// ops still collapse into one (e.g. a weight literal materialized at many sites).
    const Def* bot_mem();

    /// A *fresh* `%mem.M 0` for one emitted buffer/matrix operation: the var of a newly minted continuation
    /// `con fresh_mem(mem: %mem.M 0)` that receives its memory once LowerToMem::wrap_fresh_mem has chained it
    /// in front of the enclosing lam's body.
    /// Required by every op that allocates a buffer it then writes into, for two independent reasons:
    /// 1. Immutable Def%s are hash-consed, so two `%buffer.alloc`s agreeing on `(r, s, T)` and sharing one
    ///    placeholder collapse into a single allocation - two distinct tensors would alias one buffer.
    /// 2. AddMem is a memoizing Rewriter, so two operations sharing one argument tuple
    ///    `(⊥: %mem.M 0, …)` - which happens whenever they differ only in their curried callee - are threaded
    ///    from the *same* current memory, and the resulting parallel mem chains collapse in `%cps.conv`.
    /// Mutables are never hash-consed, so the continuations' vars are distinct by construction - no
    /// distinguishing tag required.
    const Def* fresh_mem();

    /// Chains the LowerToMem::pending_ continuations in front of @p new_lam's freshly rewritten body:
    /// `new_lam ↦ %mem.fresh (0, k₁)`, `k₁ ↦ %mem.fresh (0, k₂)`, …, and the last one carries the body.
    /// AddMem resolves each request by jumping to the continuation with the scheduler-placed current memory,
    /// and the `tt` filter beta-reduces the continuations away again as soon as that happens.
    void wrap_fresh_mem(Lam* new_lam);

    /// The fresh-memory continuations minted while the current lam's body is being rewritten.
    Vector<Lam*> pending_;

    /// Per-lam memo for the ops that consume a fresh memory (see LowerToMem::rewrite).
    DefMap<const Def*> fresh_memo_;

    /// A function is bufferized iff it is external, set, and mentions a tensor type in its domain.
    bool is_tensor_fn(Lam*) const;

    /// Whether a type is a tensor or (recursively, through sigmas and continuation domains) contains one.
    /// Never descends into `Arr` elements (so an index/shape array is not mistaken for a tensor).
    bool mentions_tensor(const Def*) const;

    DefSet tensor_ty_;

    /// Whether the program contains any (fully applied) tensor-plugin operation.
    /// Ops without any bufferized function boundary still lower: their value-world operands are
    /// materialized into buffers (see `materialize`).
    bool ops_seen_ = false;

    /// Old-world functions that get bufferized (external, signature mentions a tensor).
    /// Call sites of these functions must be adapted (see `lower_call`).
    LamSet tensor_fns_;

    /// Lams passed inside a tensor op's curry chain (combiners, affine index maps): they stay element-level —
    /// they receive element values, never buffers — even when a parameter type incidentally collides with a
    /// tensor type (pure type-based role tracking aliases, e.g. an `(x y: I32)` group *is* `«2; I32»`).
    LamSet op_args_;

};

} // namespace mim::plug::tensor::phase
