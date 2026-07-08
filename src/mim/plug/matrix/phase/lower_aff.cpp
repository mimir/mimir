#include "mim/plug/matrix/phase/lower_aff.h"

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"

#include "mim/plug/affine/affine.h"
#include "mim/plug/buffer/buffer.h"
#include "mim/plug/core/core.h"
#include "mim/plug/direct/direct.h"
#include "mim/plug/matrix/matrix.h"
#include "mim/plug/mem/mem.h"

namespace mim::plug::matrix {

namespace {

/// Builds a counting `affine.For` loop body carrying `acc` (a `{mem, …}` tuple).
std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

} // namespace

const Def* LowerAff::fold_index(const Def* shape, const Def* idx) {
    auto& w = new_world();
    auto r  = shape->num_projs();
    DefVec out;
    for (size_t i = 0; i != r; ++i)
        if (auto l = Lit::isa<u64>(shape->proj(r, i)); !(l && *l == 1)) out.push_back(idx->proj(r, i));
    return w.tuple(out);
}

const Def* LowerAff::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    if (Axm::isa<matrix::map_reduce_aff>(app)) return lower_map_reduce_aff(app);
    if (Axm::isa<matrix::broadcast>(app)) return lower_broadcast(app);
    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerAff::lower_map_reduce_aff(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs] = c->uncurry_args<7>();
    auto [To, Ro, Rr]                                             = meta->projs<3>();
    auto [So, Sr]                                                 = shapes->projs<2>();
    auto [Tis, Ris, Sis]                                          = TisRisSis->projs<3>();
    auto [comb, init]                                             = comb_init->projs<2>();

    // The final argument is `[mem, is]`; the result is `[mem, Buf]`.
    auto [op_mem, op_is] = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (Ro, So, To)]

    auto nis_l = Lit::isa<u64>(nis);
    auto ro_l = Lit::isa<u64>(Ro), rr_l = Lit::isa<u64>(Rr);
    if (!nis_l || !ro_l || !rr_l) {
        WLOG("{} doesn't have lowering-time known rank counts (nis/Ro/Rr)", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto nis_nat = *nis_l;
    auto ro = *ro_l, rr = *rr_l;
    auto nloops = ro + rr;
    auto n      = w.lit_nat(nloops);

    // Builds `%affine.map @(m, n) @(sin, sout) f idxs mem`. The map is mem-threaded (its divisions consume mem),
    // and this phase threads real memory, so the caller passes the current mem and receives `(mem', coords)`.
    auto affine_map = [&](const Def* f, const Def* m, const Def* nn, const Def* sin, const Def* sout, const Def* idxs,
                          const Def* mem) {
        auto a = w.app(w.annex<affine::map>(), w.tuple({m, nn}));
        a      = w.app(a, w.tuple({sin, sout}));
        a      = w.app(a, f);
        a      = w.app(a, idxs);
        return w.app(a, mem)->projs<2>();
    };

    auto mem_ty = w.call<mem::M>(0);

    // `[mem, is] → [mem, Buf]`, spliced via %direct.cps2ds and applied to the op's (mem, is).
    auto fun                   = w.mut_fun(w.sigma({mem_ty, op_is->type()}), result_ty)->set("mapRedAff");
    auto call                  = w.app(direct::op_cps2ds_dep(fun), w.tuple({op_mem, op_is}));
    auto [fun_mem, new_inputs] = fun->var(0_n)->projs<2>();
    auto cont                  = fun->var(1);

    // Allocate the output buffer; the output loops carry `{mem, buf}`.
    auto [obr, obs, obT]  = Axm::isa<buffer::Buf>(result_ty->proj(1))->args<3>();
    auto [a_mem, out_buf] = buffer::op_alloc(obr, obs, obT, fun_mem)->projs<2>();
    const Def* acc        = w.tuple({a_mem, out_buf});
    auto current_mut      = fun;

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
    auto [wb_mem, wb_buf] = acc->projs<2>();

    // Write-back continuation `Cn[mem, To]`.
    auto write_back                 = mem::mut_con(To)->set("writeBack");
    auto [wb_in_mem, element_final] = write_back->vars<2>();
    DefVec wb_iters                 = out_iters;
    for (u64 j = 0; j < rr; ++j)
        wb_iters.push_back(w.call(core::conv::u, Sr->proj(nloops, ro + j), w.lit(w.type_i64(), 0)));
    auto [wc_mem, write_coords] = affine_map(acc_out, Ro, n, Sr, So, w.tuple(wb_iters), wb_in_mem);
    auto stored                 = buffer::op_write(obr, obs, obT, wc_mem, wb_buf, fold_index(So, write_coords), element_final);
    write_back->app(true, cont, w.tuple({stored->proj(0), wb_buf}));

    // Reduction loops; accumulator `{mem, element}`.
    acc  = w.tuple({wb_mem, init});
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
    auto [red_mem, element_acc] = acc->projs<2>();

    DefVec iters_v = out_iters;
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    // Read one element from each input at its affine coordinates, threading memory.
    auto cur = red_mem;
    DefVec input_elements(nis_nat);
    for (u64 i = 0; i < nis_nat; ++i) {
        auto in_buf = new_inputs->proj(nis_nat, i);
        auto [mc_mem, coords]
            = affine_map(accs->proj(nis_nat, i), Ris->proj(nis_nat, i), n, Sr, Sis->proj(nis_nat, i), iters, cur);
        cur                = mc_mem;
        auto [ir, is_, iT] = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
        auto [rd_mem, rd_val]
            = buffer::op_read(ir, is_, iT, cur, in_buf, fold_index(Sis->proj(nis_nat, i), coords))->projs<2>();
        cur               = rd_mem;
        input_elements[i] = rd_val;
    }

    // The combiner is mem-threaded (`Fn [mem, To, «nis; Tis»] → [mem, To]`): call it directly, its result feeds `cont`.
    current_mut->app(true, comb, w.tuple({w.tuple({cur, element_acc, w.tuple(input_elements)}), cont}));

    return call;
}

const Def* LowerAff::lower_broadcast(const App* app) {
    auto& w              = new_world();
    auto callee          = app->callee()->as<App>(); // (broadcast {impl}) (s_in, s_out)
    auto [s_in, s_out]   = rewrite(callee->arg())->projs<2>();
    auto [op_mem, input] = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (ro, so, T)]

    auto r_nat = s_out->num_projs();

    auto mem_ty            = w.call<mem::M>(0);
    auto fun               = w.mut_fun(w.sigma({mem_ty, input->type()}), result_ty)->set("broadcast");
    auto call              = w.app(direct::op_cps2ds_dep(fun), w.tuple({op_mem, input}));
    auto [fun_mem, in_buf] = fun->var(0_n)->projs<2>();
    auto cont              = fun->var(1);

    auto [in_r, in_s, in_T]    = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
    auto [out_r, out_s, out_T] = Axm::isa<buffer::Buf>(result_ty->proj(1))->args<3>();

    auto [a_mem, out_buf] = buffer::op_alloc(out_r, out_s, out_T, fun_mem)->projs<2>();
    const Def* acc        = w.tuple({a_mem, out_buf});
    auto current_mut      = fun;
    DefVec out_iters;
    out_iters.reserve(r_nat);
    for (size_t i = 0; i < r_nat; ++i) {
        auto dim                    = s_out->proj(r_nat, i);
        auto bound                  = w.call<core::bitcast>(w.type_i64(), dim);
        auto [body, for_call]       = counting_for(bound, acc, cont, w.sym("bcast_" + std::to_string(i)));
        auto [iter, new_acc, yield] = body->vars<3>();
        cont                        = yield;
        out_iters.push_back(w.call(core::conv::u, dim, iter));
        acc = new_acc;
        current_mut->set(true, for_call);
        current_mut = body;
    }
    auto [loop_mem, loop_buf] = acc->projs<2>();

    // Non-size-1 input dims mirror the matching output index; size-1 dims are dropped from each buffer index.
    auto iters            = w.tuple(out_iters);
    auto [rd_mem, rd_val] = buffer::op_read(in_r, in_s, in_T, loop_mem, in_buf, fold_index(s_in, iters))->projs<2>();
    auto [wr_mem, wr_buf]
        = buffer::op_write(out_r, out_s, out_T, rd_mem, loop_buf, fold_index(s_out, iters), rd_val)->projs<2>();
    current_mut->app(true, cont, w.tuple({wr_mem, loop_buf}));

    return call;
}

} // namespace mim::plug::matrix
