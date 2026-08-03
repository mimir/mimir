#include "mim/plug/matrix/phase/lower_matrix_mediumlevel.h"

#include <mim/lam.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/matrix/matrix.h"

using namespace std::string_literals;

namespace mim::plug::matrix::phase {

namespace {

/// Builds a counting `affine.For` loop body carrying `acc` (a `{mem, …}` tuple), using i32 iterators.
std::pair<Lam*, const Def*> counting_for(const Def* bound, DefVec acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = w.tuple(acc)->type();
    auto body     = w.mut_con({/* iter */ w.type_i32(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i32(0), bound, w.lit_i32(1), w.tuple(acc)});
    return {body, for_loop};
}

} // namespace

const Def* LowerMatrixMediumLevel::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    auto map_reduce_ax = Axm::isa<matrix::map_reduce>(app);
    if (!map_reduce_ax) return RWPhase::rewrite_imm_App(app);

    // Meta arguments: n = out-count, S = out-dim, T = out-type, m = in-count, NI = in-dim-count, TI = types,
    // SI = dimensions.
    // Arguments: mem, zero (accumulator init), comb (combination function), inputs.
    auto [mem, zero, comb, inputs] = map_reduce_ax->args<4>();
    auto [n, S, T, m, NI, TI, SI]  = map_reduce_ax->callee()->as<App>()->args<7>();

    // Our goal is to generate a call to a function that performs:
    // ```
    // matrix = new matrix (n, S, T)
    // for out_idx {          // n for loops
    //     acc = zero
    //     for in_idx {       // remaining loops
    //         inps = read from matrices  // m-tuple
    //         acc = comb(mem, acc, inps)
    //     }
    //     write acc to output matrix
    // }
    // return matrix
    // ```

    absl::flat_hash_map<u64, const Def*> dims;     // idx ↦ nat (size bound = dimension)
    absl::flat_hash_map<u64, const Def*> iterator; // idx ↦ %Idx (S/NI#i)
    Vector<u64> out_indices;                       // output indices 0..n-1
    Vector<u64> in_indices;                        // input indices ≥ n
    Vector<DefVec> input_dims;                     // i<m ↦ j<NI#i ↦ nat (dimension SI#i#j)
    Vector<u64> n_input;                           // i<m ↦ nat (number of dimensions of SI#i)

    auto n_lit = n->isa<Lit>();
    auto m_lit = m->isa<Lit>();
    if (!n_lit || !m_lit) return RWPhase::rewrite_imm_App(app);

    auto n_nat = n_lit->get<u64>(); // number of output dimensions (in S)
    auto m_nat = m_lit->get<u64>(); // number of input matrices

    // Collect output dimensions.
    for (u64 i = 0; i < n_nat; ++i)
        dims[i] = S->proj(n_nat, i);

    // Collect the other (input) dimensions.
    for (u64 i = 0; i < m_nat; ++i) {
        auto ni_lit = Lit::isa(NI->proj(m_nat, i));
        if (!ni_lit) return RWPhase::rewrite_imm_App(app);
        u64 ni_nat = *ni_lit;
        auto SI_i  = SI->proj(m_nat, i);
        input_dims.emplace_back(DefVec(ni_nat, [&](u64 j) { return SI_i->proj(ni_nat, j); }));
        n_input.push_back(ni_nat);
    }

    // Extract the bounds for each (in/out) index.
    for (u64 i = 0; i < m_nat; ++i) {
        auto [indices, mat] = inputs->proj(m_nat, i)->projs<2>();
        for (u64 j = 0; j < n_input[i]; ++j) {
            auto idx_lit = Lit::isa(indices->proj(n_input[i], j));
            if (!idx_lit) return RWPhase::rewrite_imm_App(app);
            u64 idx_nat = *idx_lit;
            auto dim    = input_dims[i][j];
            if (!dims.contains(idx_nat)) {
                dims[idx_nat] = dim;
            } else if (auto dim_lit = dim->isa<Lit>()) { // override with more precise information
                if (auto prev_lit = dims[idx_nat]->isa<Lit>())
                    assert(dim_lit->get<u64>() == prev_lit->get<u64>() && "dimensions must be equal");
                else
                    dims[idx_nat] = dim;
            }
        }
    }

    for (auto [idx, dim] : dims)
        (idx < n_nat ? out_indices : in_indices).push_back(idx);
    // Sort the indices to make the checks below easier.
    std::sort(out_indices.begin(), out_indices.end());
    std::sort(in_indices.begin(), in_indices.end());

    // The analysis above inspected old-world defs; everything taken along into the replacement must be rewritten
    // into the new world first.
    auto& w = new_world();
    mem     = rewrite(mem);
    zero    = rewrite(zero);
    comb    = rewrite(comb);
    n       = rewrite(n);
    S       = rewrite(S);
    T       = rewrite(T);
    for (auto& [_, dim] : dims)
        dim = rewrite(dim);

    // Create a function `%mem.M 0 → [%mem.M 0, %matrix.Mat (n, S, T)]` to replace the axm call.
    auto fun  = w.mut_fun(w.call<mem::M>(0), rewrite(map_reduce_ax->type()))->set("mapRed");
    auto call = w.app(cps::op_cps2ds_dep(fun), mem);

    // Flowchart:
    // ```
    // init
    // forOut1 with yieldOut1  => exitOut1 = return_cont
    // forOut2 with yieldOut2  => exitOut2 = yieldOut1
    // ... accumulator init
    // forIn1 with yieldIn1    => exitIn1 = writeCont
    // forIn2 with yieldIn2    => exitIn2 = yieldIn1
    // ... read matrices
    // fun                     => exitFun = yieldInM
    // (return path) ... write -> yieldOutN -> ...
    // ```

    // First create the output matrix.
    auto [current_mem, init_mat] = buffer::op_alloc(n, S, T, mem)->projs<2>();

    // The continuation to continue on -- return after all output loops.
    auto cont        = fun->var(1);
    auto current_mut = fun;

    // Each outer loop carries the memory and matrix as accumulator (in an inner monad).
    DefVec acc = {current_mem, init_mat};
    for (auto idx : out_indices) {
        auto dim_nat_def = dims[idx];
        auto dim         = w.call<core::bitcast>(w.type_i32(), dim_nat_def);

        auto [body, for_call]       = counting_for(dim, acc, cont, w.sym("forIn_"s + std::to_string(idx)));
        auto [iter, new_acc, yield] = body->vars<3>();
        auto [new_mem, new_acc_val] = new_acc->projs<2>();
        cont                        = yield;
        iterator[idx]               = w.call<core::bitcast>(w.type_idx(dim_nat_def), iter);
        acc                         = {new_mem, new_acc_val};
        current_mut->set(true, for_call);
        current_mut = body;
    }

    // Now the inner loops for the inputs, carrying the elem accumulator and memory (in an inner monad).
    auto elem_acc  = zero->set("acc");
    current_mem    = acc[0];
    auto wb_matrix = acc[1];
    assert(wb_matrix);

    // Write the elem back to the matrix; this is the return after all inner loops.
    auto write_back           = mem::mut_con(T)->set("matrixWriteBack");
    auto [wb_mem, elem_final] = write_back->vars<2>();

    auto output_it_tuple = w.tuple(DefVec((size_t)n_nat, [&](u64 i) {
        auto idx = out_indices[i];
        if (idx != i) ELOG("output indices must be consecutive 0..n-1 but {} != {}", idx, i);
        assert(idx == i && "output indices must be consecutive 0..n-1");
        return iterator[idx];
    }));
    auto [wb_mem2, written_matrix]
        = buffer::op_write(n, S, T, wb_mem, wb_matrix, output_it_tuple, elem_final)->projs<2>();
    write_back->app(true, cont, {wb_mem2, written_matrix});

    // From here on the continuations take the elem and memory.
    acc  = {current_mem, elem_acc};
    cont = write_back;
    for (auto idx : in_indices) {
        auto dim_nat_def = dims[idx];
        auto dim         = w.call<core::bitcast>(w.type_i32(), dim_nat_def);

        auto [body, for_call]       = counting_for(dim, acc, cont, w.sym("forIn_"s + std::to_string(idx)));
        auto [iter, new_acc, yield] = body->vars<3>();
        auto [new_mem, new_acc_val] = new_acc->projs<2>();
        cont                        = yield;
        iterator[idx]               = w.call<core::bitcast>(w.type_idx(dim_nat_def), iter);
        acc                         = {new_mem, new_acc_val};
        current_mut->set(true, for_call);
        current_mut = body;
    }
    current_mem = acc[0];
    elem_acc    = acc[1];

    // Read one elem from each input matrix.
    DefVec input_elems((size_t)m_nat);
    for (u64 i = 0; i < m_nat; ++i) {
        auto [input_idx_tup, input_matrix] = inputs->proj(m_nat, i)->projs<2>();
        auto indices                       = input_idx_tup->projs(n_input[i]);
        auto input_it_tuple
            = w.tuple(DefVec(n_input[i], [&](u64 j) { return iterator[indices[j]->as<Lit>()->get<u64>()]; }));

        auto [new_mem, elem_i] = op_read(current_mem, rewrite(input_matrix), input_it_tuple)->projs<2>();
        current_mem            = new_mem;
        input_elems[i]         = elem_i;
    }

    current_mut->app(true, comb, {w.tuple({current_mem, elem_acc, w.tuple(input_elems)}), cont});

    return call;
}

} // namespace mim::plug::matrix::phase
