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
/// chain), and the SSA value dependencies keep them anchored and ordered. The embedded AddMemBuf phase
/// (a buffer-aware variant of `%mem.add_mem_phase`) then mem-extends all continuations and rewires every
/// memory operand to the scheduler-placed current memory — handling returns, error continuations, join
/// points, branch arms, and interleaving with a caller's own memory operations uniformly.
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
    const Def* rewrite_mut_Lam(Lam*) override;
    const Def* rewrite_imm_App(const App*) override;

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

    /// Buffer-reuse policy. `false` (the initial *always-allocate-and-copy* policy) is always sound.
    /// A future liveness-based policy may return `true` to write into the source buffer in place.
    bool reuse_in_place(const App*) const { return false; }

    /// Pre-pass: records every array type used as a tensor operand/result, the set of bufferized functions,
    /// and disables bufferization for program shapes the conversion cannot handle.
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

    /// A `⊥: %mem.M 0` placeholder consumed by emitted buffer operations; AddMemBuf replaces it with the
    /// scheduler-placed current memory.
    const Def* bot_mem();

    /// A function is bufferized iff it is external, set, and mentions a tensor type in its domain.
    bool is_tensor_fn(Lam*) const;

    /// Whether a type is a tensor or (recursively, through sigmas and continuation domains) contains one.
    /// Never descends into `Arr` elements (so an index/shape array is not mistaken for a tensor).
    bool mentions_tensor(const Def*) const;

    DefSet tensor_ty_;

    /// Old-world functions that get bufferized (external, signature mentions a tensor).
    /// Call sites of these functions must be adapted (see `lower_call`).
    LamSet tensor_fns_;

    /// Lams passed inside a tensor op's curry chain (combiners, affine index maps): they stay element-level —
    /// they receive element values, never buffers — even when a parameter type incidentally collides with a
    /// tensor type (pure type-based role tracking aliases, e.g. an `(x y: I32)` group *is* `«2; I32»`).
    LamSet op_args_;

    /// Disabled when the program contains a shape the conversion cannot handle (see `collect_tensor_types`).
    /// This phase then becomes a no-op and the value-semantics `%tensor.lower_map_reduce` handles everything instead.
    bool bufferize_ = true;
};

} // namespace mim::plug::tensor::phase
