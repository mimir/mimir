#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix::phase {

/// Lowers the low-level `map_reduce_idx` axiom into `affine.For` loops, making the iteration scheme explicit.
/// The affine-indexed sibling `map_reduce` is handled by `LowerMapReduce` instead.
///
/// Pseudo-code:
/// ```
/// out_matrix = init
/// for output_indices:
///   acc = init
///   for input_indices:
///     element_[0..m] = read(matrix[0..m], indices)
///     acc = f (acc, elements)
///   insert (out_matrix, output_indices, acc)
/// return out_matrix
/// ```
///
/// Detailed pseudo-code:
/// * out indices = (0,1,2, ..., Ro)
/// * bounds in So
/// * we assume that certain paramters are constant and statically known
///   to avoid inline-metaprogramming like multiiter
///   e.g. the number of matrices, the dimensions, the indices
/// ```
/// // iterate over out indices
/// output = init_matrix (Ro,So,To)
/// for i_0 in [0, So#0)
///   ...
///     for i_{Ro-1} in [0, So#(Ro-1))
///       s = init
///       // iterate over non-out indices
///       for j in [0, Sis#(...)]:
///         // indices depend on the specified access
///         // is#k#0
///         e_0 = read (is#0#1, (i_1, i_0))
///         ...
///         e_(nis-1) = read (is#(nis-1)#1, (i_2, j))
///
///         s = add(s, mul (e_0, ..., e_(nis-1)) )
///       write (output, (i_0, ..., i_{Ro-1}), s)
/// ```
class LowerMapReduceIdx : public RWPhase {
public:
    LowerMapReduceIdx(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::matrix::phase
