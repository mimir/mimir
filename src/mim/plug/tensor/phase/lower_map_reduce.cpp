#include "mim/plug/tensor/phase/lower_map_reduce.h"

#include "mim/def.h"
#include "mim/lam.h"

#include "mim/util/types.h"

#include "mim/plug/affine/affine.h"
#include "mim/plug/core/core.h"
#include "mim/plug/direct/direct.h"
#include "mim/plug/tensor/tensor.h"

#include "absl/container/flat_hash_map.h"

namespace mim::plug::tensor::phase {

const Def* LowerMapReduce::lower_get(const App* app) {
    auto& w  = new_world();
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [arr, index] = arg->projs<2>();
    auto callee       = c->as<App>();
    auto [T, r, s]    = callee->args<3>();

    DLOG("lower_get");
    DLOG("    arr = {} : {}", arr, arr->type());
    if (auto arr_seq = arr->type()->isa<Seq>()) DLOG("    arr shape = {}", arr_seq->arity());
    DLOG("    index = {} : {}", index, index->type());
    DLOG("    T = {} : {}", T, T->type());
    DLOG("    r = {} : {}", r, r->type());
    DLOG("    s = {} : {}", s, s->type());

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        WLOG("{} doesn't have a lowering-time known rank: {}", app, r);
        return nullptr;
    }
    if (r_nat == 1) {
        DLOG("index of size 1, extract");
        return w.extract(arr, index);
    }
    auto curr_arr = arr;
    for (auto ri = 0_u64; ri < *r_nat; ++ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    idx = {} : {}", idx, idx->type());
        curr_arr = w.extract(curr_arr, idx);
    }
    return curr_arr;
}

const Def* LowerMapReduce::lower_set(const App* app) {
    auto& w  = new_world();
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [arr, index, x] = arg->projs<3>();

    DLOG("lower_set");
    DLOG("    arr = {} : {}", arr, arr->type());
    DLOG("    index = {} : {}", index, index->type());
    DLOG("    x = {} : {}", x, x->type());

    auto callee    = c->as<App>();
    auto [T, r, s] = callee->args<3>();
    DLOG("    T = {} : {}", T, T->type());
    DLOG("    r = {} : {}", r, r->type());
    DLOG("    s = {} : {}", s, s->type());

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        WLOG("{} doesn't have a lowering-time known rank: {}", app, r);
        return nullptr;
    }
    if (r_nat == 1) {
        DLOG("index of size 1, insert");
        return w.insert(arr, index, x);
    }

    // r_nat will never be 0, as we would have normalized this case away already
    DefVec arrs_to_insert_into(*r_nat);
    arrs_to_insert_into[0] = arr;
    for (auto ri = 0_u64; ri < *r_nat - 1; ++ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    extract idx = {} : {}", idx, idx->type());
        arrs_to_insert_into[ri + 1] = w.extract(arrs_to_insert_into[ri], idx);
    }

    auto new_arr = x;
    for (auto ri = static_cast<s64>(*r_nat - 1); ri >= 0; --ri) {
        auto idx = index->proj(*r_nat, ri);
        DLOG("    idx = {} : {}", idx, idx->type());
        DLOG("    arr_to_insert_into = {} : {}", arrs_to_insert_into[ri], arrs_to_insert_into[ri]->type());

        new_arr = w.insert(arrs_to_insert_into[ri], idx, new_arr);
    }
    return new_arr;
}

const Def* LowerMapReduce::rec_broadcast(const Def* s_in, const Def* s_out, const Def* input, u64 r, u64 i) {
    auto& w = new_world();
    // Base case: all dimensions have been processed; `input` is the final scalar.
    if (i == r) return input;

    auto s_in_ri = s_in->proj(r, i), s_out_ri = s_out->proj(r, i);
    DLOG("rec_broadcast");
    DLOG("    r = {}", r);
    DLOG("    i = {}", i);
    DLOG("    s_in_ri = {} : {}", s_in_ri, s_in_ri->type());
    DLOG("    s_out_ri = {} : {}", s_out_ri, s_out_ri->type());
    DLOG("    input = {} : {}", input, input->type());

    if (s_in_ri == s_out_ri) {
        if (auto s_in_lit = Lit::isa<u64>(s_in_ri)) {
            DefVec inputs(*s_in_lit, [&](size_t j) { return rec_broadcast(s_in, s_out, input->proj(j), r, i + 1); });
            return w.tuple(inputs);
        } else {
            // TODO: we could probably support non-literal sizes as well, but we would need to generate loops to copy
            // the data instead of just packing it.
            WLOG("dimension {} of the input and output are equal but not literal: {} : {}", i, s_in_ri,
                 s_in_ri->type());
            return nullptr;
        }
    }

    if (auto s_in_lit = Lit::isa<u64>(s_in_ri); s_in_lit && *s_in_lit == 1) {
        DLOG("dimension {} of the input is 1, can be broadcasted to dimension {} of the output", i, s_out_ri);
        return w.pack(s_out_ri, rec_broadcast(s_in, s_out, input, r, i + 1));
    }

    WLOG("cannot broadcast dimension {} of size {} to size {}", i, s_in_ri, s_out_ri);
    return nullptr;
}

const Def* LowerMapReduce::lower_broadcast(const App* app) {
    auto& w  = new_world();
    auto c   = rewrite(app->callee());
    auto arg = rewrite(app->arg());

    auto [s_in, s_out, input] = arg->projs<3>();
    auto callee               = c->as<App>();
    auto [T, r]               = callee->args<2>();
    DLOG("lower_broadcast");
    DLOG("    s_out = {} : {}", s_out, s_out->type());
    DLOG("    input = {} : {}", input, input->type());
    DLOG("    T = {} : {}", T, T->type());
    DLOG("    r = {} : {}", r, r->type());
    DLOG("    s_in = {} : {}", s_in, s_in->type());

    auto r_nat = Lit::isa<u64>(r);
    if (!r_nat) {
        WLOG("{} doesn't have a lowering-time known rank: {}", app, r);
        return nullptr;
    }
    // r_nat will never be 0, as we would have normalized this case away already
    if (s_in == s_out) return input;

    if (*r_nat == 1) {
        if (auto s_in_lit = Lit::isa<u64>(s_in)) {
            assert(*s_in_lit == 1 && "input dimensions must be 1 or equal to the output dimension");
            return w.pack(s_out, input);
        }
    }

    auto result = rec_broadcast(s_in, s_out, input, *r_nat, 0);
    DLOG("result of rec_broadcast = {} : {}", result, result->type());
    return result;
}

static std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

static std::tuple<Vector<u64>, Vector<u64>, absl::flat_hash_map<u64, const Def*>, Vector<u64>>
extract_indices(const u64 n_nat, const u64 nis_nat, const Def* S, const Def* Ris, const Def* Sis, const Def* subs) {
    auto& w = S->world();

    absl::flat_hash_map<u64, const Def*> dims; // idx ↦ nat (size bound = dimension)
    Vector<u64> out_indices;                   // output indices 0..n-1
    Vector<u64> in_indices;                    // input indices ≥ n

    Vector<const Def*> output_dims; // i<n ↦ nat (dimension S#i)
    Vector<DefVec> input_dims;      // i<nis ↦ j<Ris#i ↦ nat (dimension Sis#i#j)
    Vector<u64> n_input;            // i<nis ↦ nat (number of dimensions of Sis#i)

    // collect output dimensions
    w.DLOG("out dims (n) = {}", n_nat);
    for (u64 i = 0; i < n_nat; ++i) {
        auto dim = S->proj(n_nat, i);
        w.DLOG("dim {} = {}", i, dim);
        dims[i] = dim;
        output_dims.push_back(dim);
    }

    // collect other (input) dimensions
    w.DLOG("matrix count (nis) = {}", nis_nat);

    for (u64 i = 0; i < nis_nat; ++i) {
        auto ni     = Ris->proj(nis_nat, i);
        auto ni_lit = Lit::isa(ni);
        if (!ni_lit) error("matrix {} has non-constant dimension count", i);
        u64 ni_nat = *ni_lit;
        w.DLOG("  dims({}) = {}", i, ni_nat);
        auto Sis_i = Sis->proj(nis_nat, i);
        DefVec input_dims_i;
        for (u64 j = 0; j < ni_nat; ++j) {
            auto dim = Sis_i->proj(ni_nat, j);
            w.DLOG("    dim {} {} = {}", i, j, dim);
            input_dims_i.push_back(dim);
        }
        input_dims.push_back(input_dims_i);
        n_input.push_back(ni_nat);
    }

    // extracts bounds for each index (in, out)
    for (u64 i = 0; i < nis_nat; ++i) {
        w.DLOG("investigate {} / {}", i, nis_nat);
        auto indices = subs->proj(nis_nat, i);
        w.DLOG("  indices {} = {}", i, indices);

        for (u64 j = 0; j < n_input[i]; ++j) {
            auto idx     = indices->proj(n_input[i], j);
            auto idx_lit = Lit::isa(idx);
            if (!idx_lit) error("index {} {} is not a literal", i, j);
            u64 idx_nat = *idx_lit;
            auto dim    = input_dims[i][j];
            w.DLOG("      index {} = {}", j, idx);
            w.DLOG("        dim {} = {}", idx, dim);
            if (!dims.contains(idx_nat)) {
                dims[idx_nat] = dim;
                w.DLOG("        {} ↦ {}", idx_nat, dim);
            } else {
                auto prev_dim = dims[idx_nat];
                w.DLOG("        prev dim {} = {}", idx_nat, prev_dim);
                // override with more precise information
                if (auto dim_lit = Lit::isa<u64>(dim)) {
                    if (auto prev_dim_lit = Lit::isa<u64>(prev_dim)) {
                        if (dim != prev_dim) {
                            if (!dim_lit) error("dimension {} is not a literal", dim);
                            if (!prev_dim_lit) error("previous dimension {} is not a literal", prev_dim);
                            assert(*dim_lit == *prev_dim_lit && "dimensions must be equal");
                        }
                    } else
                        dims[idx_nat] = dim;
                } else if (dim != prev_dim) {
                    error("dimensions {} and {} must be equal", dim, prev_dim);
                }
            }
        }
    }

    for (auto [idx, dim] : dims) {
        w.ILOG("dim {} = {}", idx, dim);
        if (idx < n_nat)
            out_indices.push_back(idx);
        else
            in_indices.push_back(idx);
    }
    // sort indices to make checks easier later.
    std::sort(out_indices.begin(), out_indices.end());
    std::sort(in_indices.begin(), in_indices.end());

    return {in_indices, out_indices, dims, n_input};
}

static std::tuple<const Def*, const Def*, absl::flat_hash_map<u64, const Def*>, Lam*>
create_outer_loop(Lam* fun, const Vector<u64>& out_indices, const absl::flat_hash_map<u64, const Def*>& dims) {
    auto& w = fun->world();

    // The function on where to continue -- return after all output loops.
    auto cont        = fun->var(1);
    auto current_mut = fun;

    // First create the output matrix.
    auto init_mat = w.bot(cont->type()->as<Pi>()->dom());
    w.DLOG("init_mat {} : {}", init_mat, init_mat->type());

    // Each of the outer loops contains the memory and matrix as accumulator (in an inner monad).
    auto acc = init_mat;

    absl::flat_hash_map<u64, const Def*> iterator; // idx ↦ %Idx (S/NI#i)

    for (auto idx : out_indices) {
        auto for_name    = w.sym("forIn_" + std::to_string(idx));
        auto dim_nat_def = dims.at(idx);
        auto dim         = w.call<core::bitcast>(w.type_i64(), dim_nat_def);
        w.DLOG("out_cont {} : {}", cont, cont->type());

        auto [body, for_call]       = counting_for(dim, acc, cont, for_name);
        auto [iter, new_acc, yield] = body->template vars<3>();
        cont                        = yield;
        iterator[idx]               = w.call(core::conv::u, dim_nat_def, iter);
        acc                         = new_acc;
        current_mut->set(true, for_call);
        current_mut = body;
    }
    return {acc, cont, iterator, current_mut};
}

const Def* LowerMapReduce::lower_map_reduce(const App* app) {
    // meta arguments:
    // * n = out-count, (nat)
    // * S = out-dim, (n*nat)
    // * T = out-type (*)
    // * nis = in-count (nat)
    // * Ris = in-dim-count (nis*nat)
    // * Tis = types (nis**)
    // * Sis = dimensions (nis*Ris#i)
    // arguments:
    // * mem
    // * zero = accumulator init (T)
    // * combination function (mem, acc, inputs) -> (mem, acc)
    // * input matrixes

    auto& w     = new_world();
    auto c      = rewrite(app->callee());
    auto inputs = rewrite(app->arg());
    auto type   = rewrite(app->type());
    auto callee = c->as<App>();

    auto [nis, ToRo, So, TisRisSis, comb_init, subs] = callee->uncurry_args<6>();

    auto [comb, zero]    = comb_init->projs<2>();
    auto [Tis, Ris, Sis] = TisRisSis->projs<3>();
    auto [T, n]          = ToRo->projs<2>();

    DLOG("lower map_reduce");
    DLOG("type : {}", type);
    DLOG("meta variables:");
    DLOG("  n = {}", n);
    DLOG("  S = {}", So);
    DLOG("  T = {}", T);
    DLOG("  nis = {}", nis);
    DLOG("  Ris = {} : {}", Ris, Ris->type());
    DLOG("  Tis = {} : {}", Tis, Tis->type());
    DLOG("  Sis = {} : {}", Sis, Sis->type());
    DLOG("arguments:");
    DLOG("  zero = {}", zero);
    DLOG("  comb = {} : {}", comb, comb->type());
    DLOG("  subs = {} : {}", subs, subs->type());
    DLOG("  inputs = {} : {}", inputs, inputs->type());

    // Our goal is to generate a call to a function that performs:
    // ```
    // matrix = new matrix (n, S, T)
    // for out_idx { // n for loops
    //     acc = zero
    //     for in_idx { // remaining loops
    //         inps = read from matrices // nis-tuple
    //         acc = comb(mem, acc, inps)
    //     }
    //     write acc to output matrix
    // }
    // return matrix
    // ```

    auto n_lit   = Lit::isa<u64>(n);
    auto nis_lit = Lit::isa<u64>(nis);
    if (!n_lit || !nis_lit) {
        DLOG("n or nis is not a literal");
        return nullptr;
    }

    auto n_nat   = *n_lit;   // number of output dimensions (in S)
    auto nis_nat = *nis_lit; // number of input matrices

    try {
        // out-indices are loops (potentially parallel) over the output tensor, in-indices are reductions
        auto [in_indices, out_indices, dims, n_input] = extract_indices(n_nat, nis_nat, So, Ris, Sis, subs);

        for (auto idx : out_indices)
            ILOG("output index {} with dim {}", idx, dims[idx]);
        for (auto idx : in_indices)
            ILOG("input index {} with dim {}", idx, dims[idx]);

        auto fun = w.mut_fun(inputs->type(), type)->set("mapRed");
        DLOG("fun {} : {}", fun, fun->type());

        auto ds_fun = direct::op_cps2ds_dep(fun)->set("dsFun");
        DLOG("ds_fun {} : {}", ds_fun, ds_fun->type());
        auto call = w.app(ds_fun, inputs)->set("call");
        DLOG("call {} : {}", call, call->type());

        auto new_inputs = fun->var(0)->set("is");

        DLOG("inputs = {} : {}", inputs, inputs->type());
        DLOG("new_inputs = {} : {}", new_inputs, new_inputs->type());

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

        auto [wb_matrix, cont, iterator, current_mut] = create_outer_loop(fun, out_indices, dims);

        // Now the inner loops for the inputs:
        // Each of the inner loops contains the element accumulator and memory as accumulator (in an inner monad).

        // First create the accumulator.
        auto element_acc = zero;
        element_acc->set("acc");
        assert(wb_matrix);
        DLOG("wb_matrix {} : {}", wb_matrix, wb_matrix->type());

        // Write back element to matrix. Set this as return after all inner loops.
        auto write_back = w.mut_con(T)->set("matrixWriteBack");
        DLOG("write_back {} : {}", write_back, write_back->type());
        auto element_final = write_back->var(0);

        DefVec output_iterators;
        for (u64 i = 0; i < n_nat; ++i) {
            auto idx = out_indices[i];
            if (idx != i) error("output indices must be consecutive 0..n-1 but {} != {}", idx, i);
            if (auto dim_lit = Lit::isa<u64>(dims[idx])) {
                if (*dim_lit == 1) {
                    DLOG("dimension {} is 1, no iterator needed", idx);
                    continue;
                }
            }
            output_iterators.push_back(iterator[idx]);
        }

        u64 n_oi = output_iterators.size();
        DefVec output_submatrices;
        output_submatrices.reserve(n_oi);
        output_submatrices.push_back(wb_matrix);
        for (u64 i = 0; i + 1 < n_oi; ++i)
            output_submatrices.push_back(w.extract(output_submatrices[i], output_iterators[i]));

        auto written_matrix = element_final;
        for (u64 i = 1; i <= n_oi; ++i)
            written_matrix = w.insert(output_submatrices[n_oi - i], output_iterators[n_oi - i], written_matrix);

        DLOG("written_matrix {} : {}", written_matrix, written_matrix->type());
        write_back->app(true, cont, written_matrix);

        // From here on the continuations take the element and memory.
        auto acc = element_acc;
        cont     = write_back;

        for (auto idx : in_indices) {
            auto for_name    = w.sym("forIn_" + std::to_string(idx));
            auto dim_nat_def = dims[idx];
            auto dim         = w.call<core::bitcast>(w.type_i64(), dim_nat_def);
            DLOG("in_cont {} : {}", cont, cont->type());

            auto [body, for_call]       = counting_for(dim, acc, cont, for_name);
            auto [iter, new_acc, yield] = body->vars<3>();
            cont                        = yield;
            iterator[idx]               = w.call(core::conv::u, dim_nat_def, iter);
            acc                         = new_acc;
            current_mut->set(true, for_call);
            current_mut = body;
        }
        element_acc = acc;

        // Read element from input matrix.
        DefVec input_elements((size_t)nis_nat);
        for (u64 i = 0; i < nis_nat; i++) {
            auto input_idx_tup = subs->proj(nis_nat, i);
            auto input_matrix  = new_inputs->proj(nis_nat, i);

            DLOG("input matrix {} is {} : {}", i, input_matrix, input_matrix->type());

            auto indices         = input_idx_tup->projs(n_input[i]);
            auto input_iterators = DefVec(n_input[i], [&](u64 j) {
                auto idx     = indices[j];
                auto idx_lit = Lit::isa<u64>(idx);
                DLOG("  idx {} {} = {}", i, j, *idx_lit);
                return iterator[*idx_lit];
            });

            auto curr_mat = input_matrix;
            for (auto idx : input_iterators)
                curr_mat = w.extract(curr_mat, idx);

            DLOG("read_entry {} : {}", curr_mat, curr_mat->type());
            auto element_i    = curr_mat;
            input_elements[i] = element_i;
        }

        DLOG("  read elements {}", fe::Join(input_elements));
        DLOG("  fun {} : {}", fun, fun->type());
        DLOG("  current_mut {} : {}", current_mut, current_mut->type());

        comb->set("comb");

        // TODO: make non-scalar or completely scalar?
        current_mut->app(true, comb, {w.tuple({element_acc, w.tuple(input_elements)}), cont});
        DLOG("final call {} : {}", call, call->type());
        return call;
    } catch (const std::exception& e) {
        ELOG("error during lowering map_reduce: {}", e.what());
        return nullptr;
    }
}

const Def* LowerMapReduce::lower_map_reduce_aff(const App* app) {
    // meta arguments:
    // * nis = in-count (nat)
    // * To = out-type (*), Ro = #output loops = result rank, Rr = #reduction loops
    // * So = result shape (Ro*nat)
    // * Sr = the full loop bounds (Ro+Rr)*nat: the leading Ro are the output-loop bounds, the trailing Rr the
    // reductions
    // * Tis/Ris/Sis = input types/ranks/shapes
    // arguments:
    // * f = combination function (CPS), init = accumulator init
    // * acc_out = affine map from the (Ro+Rr) loop vector to the Ro write coordinates in the result «So» (the reduction
    //             part is not in scope at write-back, so acc_out must depend only on the leading Ro output indices)
    // * accs = per-input affine map from the (Ro+Rr) loop vector to the input's read coordinates
    // * is = input tensors
    auto& w     = new_world();
    auto c      = rewrite(app->callee())->as<App>();
    auto inputs = rewrite(app->arg());
    auto type   = rewrite(app->type());

    auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs] = c->uncurry_args<7>();
    auto [To, Ro, Rr]                                             = meta->projs<3>();
    auto [So, Sr]                                                 = shapes->projs<2>();
    auto [Tis, Ris, Sis]                                          = TisRisSis->projs<3>();
    auto [comb, init]                                             = comb_init->projs<2>();

    auto nis_l = Lit::isa<u64>(nis);
    auto ro_l = Lit::isa<u64>(Ro), rr_l = Lit::isa<u64>(Rr);
    if (!nis_l || !ro_l || !rr_l) {
        WLOG("{} doesn't have lowering-time known rank counts (nis/Ro/Rr)", app);
        return nullptr;
    }
    auto nis_nat = *nis_l;
    auto ro = *ro_l, rr = *rr_l;
    auto nloops = ro + rr;           // length of the full loop vector (= length of Sr)
    auto n      = w.lit_nat(nloops); // passed as the affine maps' domain length

    // ranks of each input must be literal so that we know how many `extract`s to emit
    Vector<u64> ris_nat(nis_nat);
    for (u64 i = 0; i < nis_nat; ++i) {
        auto l = Lit::isa<u64>(Ris->proj(nis_nat, i));
        if (!l) {
            WLOG("input {} of {} has a non-literal rank", i, app);
            return nullptr;
        }
        ris_nat[i] = *l;
    }

    // Builds `%affine.map @(m, n) @(sin, sout) f idxs`. The emitted `%affine.map` is lowered to %core arithmetic by the
    // subsequent %affine.lower_index_phase.
    auto affine_map = [&](const Def* f, const Def* m, const Def* n, const Def* sin, const Def* sout, const Def* idxs) {
        auto a = w.app(w.annex<affine::map>(), w.tuple({m, n}));
        a      = w.app(a, w.tuple({sin, sout}));
        a      = w.app(a, f);
        return w.app(a, idxs);
    };
    auto nested_extract = [&](const Def* matrix, const Def* coords, u64 r) {
        auto cur = matrix;
        for (u64 k = 0; k < r; ++k)
            cur = w.extract(cur, coords->proj(r, k));
        return cur;
    };
    auto nested_insert = [&](const Def* matrix, const Def* coords, u64 r, const Def* elem) -> const Def* {
        if (r == 0) return elem;
        DefVec subs(r);
        subs[0] = matrix;
        for (u64 k = 0; k + 1 < r; ++k)
            subs[k + 1] = w.extract(subs[k], coords->proj(r, k));
        auto cur = elem;
        for (auto k = static_cast<s64>(r) - 1; k >= 0; --k)
            cur = w.insert(subs[k], coords->proj(r, k), cur);
        return cur;
    };

    try {
        auto fun    = w.mut_fun(inputs->type(), type)->set("mapRedAff");
        auto ds_fun = direct::op_cps2ds_dep(fun)->set("dsFun");
        auto call   = w.app(ds_fun, inputs)->set("call");

        auto new_inputs = fun->var(0)->set("is");

        // Outer (parallel) loops over the leading Ro bounds of `Sr`, collecting the output iteration indices.
        auto cont        = fun->var(1);
        auto init_mat    = w.bot(cont->type()->as<Pi>()->dom());
        auto acc         = init_mat;
        auto current_mut = fun;
        DefVec out_iters;
        out_iters.reserve(ro);
        for (u64 i = 0; i < ro; ++i) {
            auto dim                    = Sr->proj(nloops, i);
            auto bound                  = w.call<core::bitcast>(w.type_i64(), dim);
            auto [body, for_call]       = counting_for(bound, acc, cont, w.sym("forOut_" + std::to_string(i)));
            auto [iter, new_acc, yield] = body->vars<3>();
            cont                        = yield;
            out_iters.push_back(w.call(core::conv::u, dim, iter));
            acc = new_acc;
            current_mut->set(true, for_call);
            current_mut = body;
        }
        auto wb_matrix = acc;

        // Write-back: narrow the accumulated element into the result at the affine write coordinates `acc_out`.
        // acc_out takes the full (Ro+Rr) loop vector, but the reduction loops have already been folded away here, so we
        // pass 0 for those slots; acc_out must depend only on the leading Ro output indices.
        auto write_back    = w.mut_con(To)->set("writeBack");
        auto element_final = write_back->var(0);
        DefVec wb_iters    = out_iters;
        for (u64 j = 0; j < rr; ++j)
            wb_iters.push_back(w.call(core::conv::u, Sr->proj(nloops, ro + j), w.lit(w.type_i64(), 0)));
        auto write_coords = affine_map(acc_out, Ro, n, Sr, So, w.tuple(wb_iters)); // «Ro; Idx (So#k)»
        write_back->app(true, cont, nested_insert(wb_matrix, write_coords, ro, element_final));

        // Inner (reduction) loops over the trailing Rr bounds of `Sr`, collecting the reduction iteration indices.
        acc  = init;
        cont = write_back;
        DefVec red_iters;
        red_iters.reserve(rr);
        for (u64 j = 0; j < rr; ++j) {
            auto dim                    = Sr->proj(nloops, ro + j);
            auto bound                  = w.call<core::bitcast>(w.type_i64(), dim);
            auto [body, for_call]       = counting_for(bound, acc, cont, w.sym("forIn_" + std::to_string(j)));
            auto [iter, new_acc, yield] = body->vars<3>();
            cont                        = yield;
            red_iters.push_back(w.call(core::conv::u, dim, iter));
            acc = new_acc;
            current_mut->set(true, for_call);
            current_mut = body;
        }
        auto element_acc = acc;

        // The full loop iteration vector `(o…, r…)`; its moduli are exactly `Sr`.
        DefVec iters_v = out_iters;
        iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
        auto iters = w.tuple(iters_v);

        // Read one element from each input at its affine read coordinates.
        DefVec input_elements(nis_nat);
        for (u64 i = 0; i < nis_nat; ++i) {
            auto input_matrix = new_inputs->proj(nis_nat, i);
            auto coords
                = affine_map(accs->proj(nis_nat, i), Ris->proj(nis_nat, i), n, Sr, Sis->proj(nis_nat, i), iters);
            input_elements[i] = nested_extract(input_matrix, coords, ris_nat[i]);
        }

        comb->set("comb");
        current_mut->app(true, comb, {w.tuple({element_acc, w.tuple(input_elements)}), cont});
        return call;
    } catch (const std::exception& e) {
        error("error during lowering map_reduce_aff: {}", e.what());
    }
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (auto get = Axm::isa<tensor::get>(app)) {
        if (auto res = lower_get(get)) return res;
    } else if (auto set = Axm::isa<tensor::set>(app)) {
        if (auto res = lower_set(set)) return res;
    } else if (auto bc = Axm::isa<tensor::broadcast>(app)) {
        if (auto res = lower_broadcast(bc)) return res;
    } else if (auto mr = Axm::isa<tensor::map_reduce>(app)) {
        if (auto res = lower_map_reduce(mr)) return res;
    } else if (auto mra = Axm::isa<tensor::map_reduce_aff>(app)) {
        if (auto res = lower_map_reduce_aff(mra)) return res;
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
