#include "mim/plug/matrix/pass/lower_matrix_mediumlevel.h"

#include <iostream>

#include <mim/lam.h>

#include "mim/def.h"

#include "mim/plug/affine/affine.h"
#include "mim/plug/core/core.h"
#include "mim/plug/direct/direct.h"
#include "mim/plug/matrix/matrix.h"
#include "mim/plug/mem/mem.h"

using namespace std::string_literals;

namespace mim::plug::matrix {

const Def* LowerMatrixMediumLevel::rewrite(const Def* def) {
    if (auto i = rewritten.find(def); i != rewritten.end()) return i->second;
    auto new_def   = rewrite_(def);
    rewritten[def] = new_def;
    return rewritten[def];
}

std::pair<Lam*, const Def*> counting_for(const Def* bound, DefVec acc, const Def* exit, Sym name) {
    auto& world = bound->world();
    auto acc_ty = world.tuple(acc)->type();
    auto body
        = world.mut_con({/* iter */ world.type_i32(), /* acc */ acc_ty, /* return */ world.cn(acc_ty)})->set(name);
    auto for_loop
        = world.call<affine::For>(body, exit, Defs{world.lit_i32(0), bound, world.lit_i32(1), world.tuple(acc)});
    return {body, for_loop};
}

// TODO: compare with other impala version (why is one easier than the other?)
// TODO: replace sum_ptr by using sum as accumulator
// TODO: extract inner loop into function (for read normalizer)
const Def* LowerMatrixMediumLevel::rewrite_(const Def* def) {
    if (auto map_reduce_ax = Axm::isa<matrix::map_reduce>(def); map_reduce_ax) {
        // meta arguments:
        // * n = out-count, (nat)
        // * S = out-dim, (n*nat)
        // * T = out-type (*)
        // * m = in-count (nat)
        // * NI = in-dim-count (m*nat)
        // * TI = types (m**)
        // * SI = dimensions (m*NI#i)
        // arguments:
        // * mem
        // * zero = accumulator init (T)
        // * combination function (mem, acc, inputs) -> (mem, acc)
        // * input matrixes
        auto [mem, zero, comb, inputs] = map_reduce_ax->args<4>();
        auto [n, S, T, m, NI, TI, SI]  = map_reduce_ax->callee()->as<App>()->args<7>();
        DLOG("map_reduce_ax {} : {}", map_reduce_ax, map_reduce_ax->type());
        DLOG("meta variables:");
        DLOG("  n = {}", n);
        DLOG("  S = {}", S);
        DLOG("  T = {}", T);
        DLOG("  m = {}", m);
        DLOG("  NI = {} : {}", NI, NI->type());
        DLOG("  TI = {} : {}", TI, TI->type());
        DLOG("  SI = {} : {}", SI, SI->type());
        DLOG("arguments:");
        DLOG("  mem = {}", mem);
        DLOG("  zero = {}", zero);
        DLOG("  comb = {} : {}", comb, comb->type());
        DLOG("  inputs = {} : {}", inputs, inputs->type());

        // Our goal is to generate a call to a function that performs:
        // ```
        // matrix = new matrix (n, S, T)
        // for out_idx { // n for loops
        //     acc = zero
        //     for in_idx { // remaining loops
        //         inps = read from matrices // m-tuple
        //         acc = comb(mem, acc, inps)
        //     }
        //     write acc to output matrix
        // }
        // return matrix
        // ```

        absl::flat_hash_map<u64, const Def*> dims;         // idx ↦ nat (size bound = dimension)
        absl::flat_hash_map<u64, const Def*> raw_iterator; // idx ↦ I32
        absl::flat_hash_map<u64, const Def*> iterator;     // idx ↦ %Idx (S/NI#i)
        Vector<u64> out_indices;                           // output indices 0..n-1
        Vector<u64> in_indices;                            // input indices ≥ n

        Vector<const Def*> output_dims; // i<n ↦ nat (dimension S#i)
        Vector<DefVec> input_dims;      // i<m ↦ j<NI#i ↦ nat (dimension SI#i#j)
        Vector<u64> n_input;            // i<m ↦ nat (number of dimensions of SI#i)

        auto n_lit = n->isa<Lit>();
        auto m_lit = m->isa<Lit>();
        if (!n_lit || !m_lit) {
            DLOG("n or m is not a literal");
            return def;
        }

        auto n_nat = n_lit->get<u64>(); // number of output dimensions (in S)
        auto m_nat = m_lit->get<u64>(); // number of input matrices

        // collect output dimensions
        DLOG("out dims (n) = {}", n_nat);
        for (u64 i = 0; i < n_nat; ++i) {
            auto dim = S->proj(n_nat, i);
            DLOG("dim {} = {}", i, dim);
            dims[i] = dim;
            output_dims.push_back(dim);
        }

        // collect other (input) dimensions
        DLOG("matrix count (m) = {}", m_nat);

        for (u64 i = 0; i < m_nat; ++i) {
            auto ni     = NI->proj(m_nat, i);
            auto ni_lit = Lit::isa(ni);
            if (!ni_lit) {
                DLOG("matrix {} has non-constant dimension count", i);
                return def;
            }
            u64 ni_nat = *ni_lit;
            DLOG("  dims({}) = {}", i, ni_nat);
            auto SI_i = SI->proj(m_nat, i);
            DefVec input_dims_i;
            for (u64 j = 0; j < ni_nat; ++j) {
                auto dim = SI_i->proj(ni_nat, j);
                DLOG("    dim {} {} = {}", i, j, dim);
                // dims[i * n_nat + j] = dim;
                input_dims_i.push_back(dim);
            }
            input_dims.push_back(input_dims_i);
            n_input.push_back(ni_nat);
        }

        // extracts bounds for each index (in, out)
        for (u64 i = 0; i < m_nat; ++i) {
            DLOG("investigate {} / {}", i, m_nat);
            auto [indices, mat] = inputs->proj(m_nat, i)->projs<2>();
            DLOG("  indices {} = {}", i, indices);
            DLOG("  matrix {} = {}", i, mat);
            for (u64 j = 0; j < n_input[i]; ++j) {
                // DLOG("    dimension {} / {}", j, n_input[i]);
                auto idx     = indices->proj(n_input[i], j);
                auto idx_lit = Lit::isa(idx);
                if (!idx_lit) {
                    DLOG("    index {} {} is not a literal", i, j);
                    return def;
                }
                u64 idx_nat = *idx_lit;
                auto dim    = input_dims[i][j];
                DLOG("      index {} = {}", j, idx);
                DLOG("        dim {} = {}", idx, dim);
                if (!dims.contains(idx_nat)) {
                    dims[idx_nat] = dim;
                    DLOG("        {} ↦ {}", idx_nat, dim);
                } else {
                    // assert(dims[idx_nat] == dim);
                    auto prev_dim = dims[idx_nat];
                    DLOG("        prev dim {} = {}", idx_nat, prev_dim);
                    // override with more precise information
                    if (auto dim_lit = dim->isa<Lit>()) {
                        if (auto prev_dim_lit = prev_dim->isa<Lit>())
                            assert(dim_lit->get<u64>() == prev_dim_lit->get<u64>() && "dimensions must be equal");
                        else
                            dims[idx_nat] = dim;
                    }
                }
            }
        }

        for (auto [idx, dim] : dims) {
            DLOG("dim {} = {}", idx, dim);
            if (idx < n_nat)
                out_indices.push_back(idx);
            else
                in_indices.push_back(idx);
        }
        // sort indices to make checks easier later.
        std::sort(out_indices.begin(), out_indices.end());
        std::sort(in_indices.begin(), in_indices.end());

        // create function `%mem.M 0 -> [%mem.M 0, %matrix.Mat (n,S,T)]` to replace axm call

        auto mem_type = world().call<mem::M>(0);
        auto fun      = world().mut_fun(mem_type, map_reduce_ax->type())->set("mapRed");

        // assert(0);
        auto ds_fun = direct::op_cps2ds_dep(fun);
        DLOG("ds_fun {} : {}", ds_fun, ds_fun->type());
        auto call = world().app(ds_fun, mem);
        DLOG("call {} : {}", call, call->type());

        // flowchart:
        // ```
        // -> init
        // -> forOut1 with yieldOut1
        //    => exitOut1 = return_cont
        // -> forOut2 with yieldOut2
        //    => exitOut2 = yieldOut1
        // -> ...
        // -> accumulator init
        // -> forIn1 with yieldIn1
        //    => exitIn1 = writeCont
        // -> forIn2 with yieldIn2
        //    => exitIn2 = yieldIn1
        // -> ...
        // -> read matrices
        // -> fun
        //    => exitFun = yieldInM
        //
        // (return path)
        // -> ...
        // -> write
        // -> yieldOutN
        // -> ...
        // ```

        // First create the output matrix.
        auto current_mem      = mem;
        auto [mem2, init_mat] = world().app(world().annex<matrix::init>(), {n, S, T, current_mem})->projs<2>();
        current_mem           = mem2;

        // The function on where to continue -- return after all output loops.
        auto cont        = fun->var(1);
        auto current_mut = fun;

        // Each of the outer loops contains the memory and matrix as accumulator (in an inner monad).
        DefVec acc = {current_mem, init_mat};

        for (auto idx : out_indices) {
            auto for_name    = world().sym("forIn_"s + std::to_string(idx));
            auto dim_nat_def = dims[idx];
            auto dim         = world().call<core::bitcast>(world().type_i32(), dim_nat_def);

            auto [body, for_call]       = counting_for(dim, acc, cont, for_name);
            auto [iter, new_acc, yield] = body->vars<3>();
            cont                        = yield;
            raw_iterator[idx]           = iter;
            iterator[idx]               = world().call<core::bitcast>(world().type_idx(dim_nat_def), iter);
            auto [new_mem, new_mat]     = new_acc->projs<2>();
            acc                         = {new_mem, new_mat};
            current_mut->set(true, for_call);
            current_mut = body;
        }

        // Now the inner loops for the inputs:
        // Each of the inner loops contains the element accumulator and memory as accumulator (in an inner monad).
        DLOG("acc at inner: {}", fe::Join(acc, ";"));

        // First create the accumulator.
        auto element_acc = zero;
        element_acc->set("acc");
        current_mem    = acc[0];
        auto wb_matrix = acc[1];
        assert(wb_matrix);
        DLOG("wb_matrix {} : {}", wb_matrix, wb_matrix->type());

        // Write back element to matrix. Set this as return after all inner loops.
        auto write_back = mem::mut_con(T)->set("matrixWriteBack");
        DLOG("write_back {} : {}", write_back, write_back->type());
        auto [wb_mem, element_final] = write_back->vars<2>();

        auto output_iterators = DefVec((size_t)n_nat, [&](u64 i) {
            auto idx = out_indices[i];
            if (idx != i) ELOG("output indices must be consecutive 0..n-1 but {} != {}", idx, i);
            assert(idx == i && "output indices must be consecutive 0..n-1");
            auto iter_idx_def = iterator[idx];
            return iter_idx_def;
        });
        auto output_it_tuple  = world().tuple(output_iterators);
        DLOG("output tuple: {} : {}", output_it_tuple, output_it_tuple->type());

        auto [wb_mem2, written_matrix] = world()
                                             .app(world().app(world().annex<matrix::insert>(), {n, S, T}),
                                                  {wb_mem, wb_matrix, output_it_tuple, element_final})
                                             ->projs<2>();

        write_back->app(true, cont, {wb_mem2, written_matrix});

        // From here on the continuations take the element and memory.
        acc  = {current_mem, element_acc};
        cont = write_back;

        // TODO this is copy&paste code from above
        for (auto idx : in_indices) {
            auto for_name    = world().sym("forIn_"s + std::to_string(idx));
            auto dim_nat_def = dims[idx];
            auto dim         = world().call<core::bitcast>(world().type_i32(), dim_nat_def);

            auto [body, for_call]       = counting_for(dim, acc, cont, for_name);
            auto [iter, new_acc, yield] = body->vars<3>();
            cont                        = yield;
            raw_iterator[idx]           = iter;
            iterator[idx]               = world().call<core::bitcast>(world().type_idx(dim_nat_def), iter);
            auto [new_mem, new_element] = new_acc->projs<2>();
            acc                         = {new_mem, new_element};
            current_mut->set(true, for_call);
            current_mut = body;
        }

        // For testing: id in innermost loop instead of read, fun:
        // current_mut->app(true, cont, acc);

        current_mem = acc[0];
        element_acc = acc[1];

        // Read element from input matrix.
        DefVec input_elements((size_t)m_nat);
        for (u64 i = 0; i < m_nat; i++) {
            // TODO: case m_nat == 1
            auto input_i                       = inputs->proj(m_nat, i);
            auto [input_idx_tup, input_matrix] = input_i->projs<2>();

            DLOG("input matrix {} is {} : {}", i, input_matrix, input_matrix->type());

            auto indices         = input_idx_tup->projs(n_input[i]);
            auto input_iterators = DefVec(n_input[i], [&](u64 j) {
                auto idx     = indices[j];
                auto idx_lit = idx->as<Lit>()->get<u64>();
                DLOG("  idx {} {} = {}", i, j, idx_lit);
                return iterator[idx_lit];
            });
            auto input_it_tuple  = world().tuple(input_iterators);

            auto read_entry = op_read(current_mem, input_matrix, input_it_tuple);
            DLOG("read_entry {} : {}", read_entry, read_entry->type());
            auto [new_mem, element_i] = read_entry->projs<2>();
            current_mem               = new_mem;
            input_elements[i]         = element_i;
        }

        DLOG("  read elements {}", fe::Join(input_elements));
        DLOG("  fun {} : {}", fun, fun->type());

        // TODO: make non-scalar or completely scalar?
        current_mut->app(true, comb, {world().tuple({current_mem, element_acc, world().tuple(input_elements)}), cont});

        return call;
    }

    return def;
}

} // namespace mim::plug::matrix
