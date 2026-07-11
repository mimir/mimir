#pragma once

#include <mim/def.h>
#include <mim/phase.h>

namespace mim::plug::matrix {

/// In this step, we lower `map_reduce` operations into affine for loops making the iteration scheme explicit.
/// Pseudo-code:
/// ```
/// out_matrix = init
/// for output_indices:
///   acc = zero
///   for input_indices:
///     element_[0..m] = read(matrix[0..m], indices)
///     acc = f (acc, elements)
///   insert (out_matrix, output_indices, acc)
/// return out_matrix
/// ```
///
/// Detailed pseudo-code:
/// * out indices = (0,1,2, ..., n)
/// * bounds in S
/// * we assume that certain paramters are constant and statically known
///   to avoid inline-metaprogramming like multiiter
///   e.g. the number of matrizes, the dimensions, the indices
/// ```
/// // iterate over out indices
/// output = init_matrix (n,S,T)
/// for i_0 in [0, S#0)
///   ...
///     for i_{n-1} in [0, S#(n-1))
///       s = zero
///       // iterate over non-out indices
///       for j in [0, SI#(...)]:
///         // indices depend on the specified access
///         // input#k#0
///         e_0 = read (input#0#1, (i_1, i_0))
///         ...
///         e_(m-1) = read (input#(m-1)#1, (i_2, j))
///
///         s = add(s, mul (e_0, ..., e_(m-1)) )
///       write (output, (i_0, ..., i_{n-1}), s)
/// ```
class LowerMatrixMediumLevel : public RWPhase {
public:
    LowerMatrixMediumLevel(World& world, flags_t annex)
        : RWPhase(world, annex) {}

    const Def* rewrite_imm_App(const App*) final;
};

} // namespace mim::plug::matrix
