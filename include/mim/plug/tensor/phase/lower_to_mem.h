#pragma once

#include <mim/def.h>
#include <mim/lam.h>
#include <mim/phase.h>

namespace mim::plug::tensor::phase {

/// Bufferizes the low-level tensor axioms onto the shared `buffer` layer.
/// `get` / `set` / `map_reduce_aff` become `%buffer.read` / `%buffer.write` / `%buffer.alloc`,
/// and tensor array values `«s; T»` become `%buffer.Buf (r, s, T)` handles.
/// Afterwards `%buffer.lower_ptr` lowers the buffer layer to `%mem.Ptr` + `%mem.lea` / `%mem.load` / `%mem.store`.
///
/// Because the tensor operations are pure (SSA), bufferization introduces side effects, so this phase also
/// threads the `%mem.M` memory monad: tensor functions gain a leading `%mem.M 0` parameter and return it, and the
/// memory is threaded through the emitted buffer operations.
///
/// Which array types denote tensors (as opposed to index/shape arrays that share the `Arr` structure) is
/// determined by *role*: a pre-pass collects the array operand/result types of the tensor operations, and only
/// those types are rewritten to `Buf`.
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
    const Def* lower_map_reduce_aff(const App*);

    /// Buffer-reuse policy. `false` (the initial *always-allocate-and-copy* policy) is always sound.
    /// A future liveness-based policy may return `true` to write into the source buffer in place.
    bool reuse_in_place(const App*) const { return false; }

    /// Pre-pass: records every array type used as a tensor operand/result.
    void collect_tensor_types();

    /// Builds the `%buffer.Buf` type for an old tensor array type `«s; T»` (peeling the nested `Arr`s).
    const Def* buf_of(const Def* arr_ty);

    /// Drops, from the (unfolded) index `idx`, the components of size-1 dimensions of `shape`.
    /// MimIR folds size-1 dimensions out of array/buffer types (`«3,1;T»` ≡ `«3;T»`), so an index addressing a
    /// buffer must match the folded shape.
    const Def* fold_index(const Def* shape, const Def* idx);

    /// A function is bufferized (and mem-threaded) iff it is external and mentions a tensor type.
    bool is_tensor_fn(Lam*) const;

    /// Whether a type is a tensor or (recursively, through sigmas and continuation domains) contains one.
    /// Never descends into `Arr` elements (so an index/shape array is not mistaken for a tensor).
    bool mentions_tensor(const Def*) const;

    GIDSet<const Def*> tensor_ty_;

    /// Memory threaded through the function currently being rewritten, and that function's old return var.
    const Def* cur_mem_     = nullptr;
    const Def* cur_ret_old_ = nullptr;
};

} // namespace mim::plug::tensor::phase
