#include "mim/plug/tensor/phase/lower_to_mem.h"

#include <queue>

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"

#include "mim/plug/affine/affine.h"
#include "mim/plug/buffer/buffer.h"
#include "mim/plug/core/core.h"
#include "mim/plug/direct/direct.h"
#include "mim/plug/mem/mem.h"
#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

/// Builds a counting `affine.For` loop body carrying `acc` (which may be a `{mem, …}` tuple).
/// Mirrors `tensor::phase::LowerMapReduce`'s helper but is kept local to the bufferizing phase.
std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

} // namespace

void LowerToMem::collect_tensor_types() {
    GIDSet<const Def*> visited;
    std::queue<const Def*> wl;
    auto push = [&](const Def* d) {
        if (d) wl.push(d);
    };

    for (auto mut : old_world().externals().muts()) push(mut);

    while (!wl.empty()) {
        auto def = wl.front();
        wl.pop();
        if (!visited.emplace(def).second) continue;

        if (auto app = def->isa<App>()) {
            if (Axm::isa<tensor::get>(app) || Axm::isa<tensor::set>(app)) {
                // get/set: the first explicit argument is the tensor `arr`.
                tensor_ty_.emplace(app->arg()->proj(0)->type());
            } else if (Axm::isa<tensor::map_reduce_aff>(app)) {
                // result and each of the `nis` inputs are tensors.
                tensor_ty_.emplace(app->type());
                auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs] = app->callee()->as<App>()->uncurry_args<7>();
                if (auto nis_l = Lit::isa<u64>(nis))
                    for (u64 i = 0; i < *nis_l; ++i) tensor_ty_.emplace(app->arg()->proj(*nis_l, i)->type());
            } else if (Axm::isa<tensor::broadcast>(app)) {
                // result «s_out; T» and input «s_in; T» (the 3rd argument) are tensors.
                tensor_ty_.emplace(app->type());
                tensor_ty_.emplace(app->arg()->proj(2)->type());
            }
        }

        for (auto op : def->ops()) push(op);
        push(def->type());
    }
}

void LowerToMem::start() {
    collect_tensor_types();
    RWPhase::start();
}

const Def* LowerToMem::buf_of(const Def* arr_ty) {
    auto& w = new_world();
    DefVec dims;
    auto cur = arr_ty;
    while (auto arr = cur->isa<Arr>()) {
        dims.push_back(rewrite(arr->arity()));
        cur = arr->body();
    }
    return buffer::type_buf(w.lit_nat(dims.size()), w.tuple(dims), rewrite(cur));
}

const Def* LowerToMem::fold_index(const Def* shape, const Def* idx) {
    auto& w = new_world();
    auto r  = shape->num_projs();
    DefVec out;
    for (size_t i = 0; i != r; ++i)
        if (auto l = Lit::isa<u64>(shape->proj(r, i)); !(l && *l == 1)) out.push_back(idx->proj(r, i));
    return w.tuple(out);
}

bool LowerToMem::mentions_tensor(const Def* t) const {
    if (tensor_ty_.contains(t)) return true;
    if (auto sig = t->isa<Sigma>()) {
        for (auto op : sig->ops())
            if (mentions_tensor(op)) return true;
    } else if (auto pi = t->isa<Pi>()) {
        return mentions_tensor(pi->dom()); // descend into continuation domains, but never into `Arr` elements
    }
    return false;
}

bool LowerToMem::is_tensor_fn(Lam* lam) const {
    return lam->is_external() && lam->is_set() && mentions_tensor(lam->type()->dom());
}

const Def* LowerToMem::rewrite_mut_Lam(Lam* lam) {
    if (is_bootstrapping() || !is_tensor_fn(lam)) return RWPhase::rewrite_mut_Lam(lam);

    auto& w     = new_world();
    auto mem_ty = w.call<mem::M>(0);
    auto dom    = lam->type()->dom();
    auto n      = dom->num_projs();

    // New domain: leading `%mem.M 0`, then the (bufferized) parameters, with the return continuation
    // also extended by a leading `%mem.M 0`. Only tensor-typed positions become `%buffer.Buf`; other
    // arrays (e.g. index/shape tuples or an incidental `«2; I32»` operand pair) are left untouched.
    auto conv = [&](const Def* t) { return tensor_ty_.contains(t) ? buf_of(t) : rewrite(t); };
    DefVec doms;
    doms.push_back(mem_ty);
    for (size_t i = 0; i != n; ++i) {
        auto d = dom->proj(n, i);
        if (auto pi = Pi::isa_cn(d))
            doms.push_back(w.cn({mem_ty, conv(pi->dom())}));
        else
            doms.push_back(conv(d));
    }

    auto new_lam = w.mut_con(doms)->set(lam->dbg());
    map(lam, new_lam);
    for (size_t i = 0; i != lam->num_vars(); ++i) map(lam->var(i), new_lam->var(i + 1));

    auto save_mem = cur_mem_;
    auto save_ret = cur_ret_old_;
    cur_mem_      = new_lam->var(0_n);
    cur_ret_old_  = lam->ret_var();

    auto filter = rewrite(lam->filter());
    auto body   = rewrite(lam->body());
    new_lam->set(filter, body);

    cur_mem_     = save_mem;
    cur_ret_old_ = save_ret;
    return new_lam;
}

const Def* LowerToMem::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    if (Axm::isa<tensor::get>(app)) return lower_get(app);
    if (Axm::isa<tensor::set>(app)) return lower_set(app);
    if (Axm::isa<tensor::broadcast>(app)) return lower_broadcast(app);
    if (Axm::isa<tensor::map_reduce_aff>(app)) return lower_map_reduce_aff(app);

    // Exit of a bufferized function: thread the accumulated memory into the return.
    if (cur_ret_old_ && app->callee() == cur_ret_old_) {
        auto& w      = new_world();
        auto new_ret = rewrite(app->callee());
        auto new_res = rewrite(app->arg());
        return w.app(new_ret, w.tuple({cur_mem_, new_res}));
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerToMem::lower_get(const App* app) {
    auto c             = rewrite(app->callee())->as<App>();
    auto arg           = rewrite(app->arg());
    auto [arr, index]  = arg->projs<2>();
    auto [T, r, s]     = c->args<3>();
    auto [br, bs, bT]  = Axm::isa<buffer::Buf>(arr->type())->args<3>(); // actual (folded) buffer metadata

    auto read = buffer::op_read(br, bs, bT, cur_mem_, arr, fold_index(s, index));
    cur_mem_  = read->proj(0);
    return read->proj(1); // the loaded value
}

const Def* LowerToMem::lower_set(const App* app) {
    auto c               = rewrite(app->callee())->as<App>();
    auto arg             = rewrite(app->arg());
    auto [arr, index, x] = arg->projs<3>();
    auto [T, r, s]       = c->args<3>();
    auto [br, bs, bT]    = Axm::isa<buffer::Buf>(arr->type())->args<3>(); // actual (folded) buffer metadata
    auto fidx            = fold_index(s, index);

    if (reuse_in_place(app)) {
        auto written = buffer::op_write(br, bs, bT, cur_mem_, arr, fidx, x);
        cur_mem_     = written->proj(0);
        return written->proj(1);
    }

    // AlwaysAllocate policy: allocate a fresh buffer, copy the source in, then write the element.
    auto [m1, q] = buffer::op_alloc(br, bs, bT, cur_mem_)->projs<2>();
    auto m2      = buffer::op_copy(br, bs, bT, m1, q, arr);
    auto written = buffer::op_write(br, bs, bT, m2, q, fidx, x);
    cur_mem_     = written->proj(0);
    return written->proj(1);
}

const Def* LowerToMem::lower_broadcast(const App* app) {
    auto& w                   = new_world();
    auto c                    = rewrite(app->callee())->as<App>();
    auto arg                  = rewrite(app->arg());
    auto [s_in, s_out, input] = arg->projs<3>();
    auto [T, r]               = c->args<2>();

    // No-op broadcast (already normalized away in most cases).
    if (s_in == s_out) return input;

    auto r_l = Lit::isa<u64>(r);
    if (!r_l) {
        WLOG("{} doesn't have a lowering-time known rank", app);
        return nullptr;
    }
    auto r_nat = *r_l;

    auto mem_ty            = w.call<mem::M>(0);
    auto out_ty            = buf_of(app->type()); // the folded result buffer type
    auto fun               = w.mut_fun(w.sigma({mem_ty, input->type()}), w.sigma({mem_ty, out_ty}))->set("broadcast");
    auto call              = w.app(direct::op_cps2ds_dep(fun), w.tuple({cur_mem_, input}));
    auto [fun_mem, in_buf] = fun->var(0_n)->projs<2>();
    auto cont              = fun->var(1);

    // The actual (folded) buffer metadata is read off the buffer types, not the op's unfolded shapes.
    auto [in_r, in_s, in_T]    = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
    auto [out_r, out_s, out_T] = Axm::isa<buffer::Buf>(out_ty)->args<3>();

    // Loop over the (unfolded) output shape, carrying `{mem, out_buf}`.
    auto [a_mem, out_buf] = buffer::op_alloc(out_r, out_s, out_T, fun_mem)->projs<2>();
    const Def* acc        = w.tuple({a_mem, out_buf});
    auto current_mut      = fun;
    DefVec out_iters;
    out_iters.reserve(r_nat);
    for (u64 i = 0; i < r_nat; ++i) {
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

    // Read the input at the broadcast index (non-size-1 input dims mirror the matching output index), then write.
    auto iters = w.tuple(out_iters);
    auto rd    = buffer::op_read(in_r, in_s, in_T, loop_mem, in_buf, fold_index(s_in, iters));
    auto wr = buffer::op_write(out_r, out_s, out_T, rd->proj(0), loop_buf, fold_index(s_out, iters), rd->proj(1));
    current_mut->app(true, cont, w.tuple({wr->proj(0), loop_buf}));

    cur_mem_ = call->proj(0);
    return call->proj(1);
}

const Def* LowerToMem::lower_map_reduce_aff(const App* app) {
    auto& w     = new_world();
    auto c      = rewrite(app->callee())->as<App>();
    auto inputs = rewrite(app->arg());
    auto type   = buf_of(app->type()); // the (bufferized) result `%buffer.Buf (Ro, So, To)`

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
    auto nloops = ro + rr;
    auto n      = w.lit_nat(nloops);

    Vector<u64> ris_nat(nis_nat);
    for (u64 i = 0; i < nis_nat; ++i) {
        auto l = Lit::isa<u64>(Ris->proj(nis_nat, i));
        if (!l) {
            WLOG("input {} of {} has a non-literal rank", i, app);
            return nullptr;
        }
        ris_nat[i] = *l;
    }

    // `%affine.map @(m, n) @(sin, sout) f idxs` — lowered to %core arithmetic by %affine.lower_index_phase.
    auto affine_map = [&](const Def* f, const Def* m, const Def* nn, const Def* sin, const Def* sout, const Def* idxs) {
        auto a = w.app(w.annex<affine::map>(), w.tuple({m, nn}));
        a      = w.app(a, w.tuple({sin, sout}));
        a      = w.app(a, f);
        return w.app(a, idxs);
    };

    auto mem_ty = w.call<mem::M>(0);

    // The map_reduce becomes a function `[mem, inputs] → [mem, result_buf]`, spliced into the expression via
    // %direct.cps2ds and applied to the current memory + inputs.
    auto fun     = w.mut_fun(w.sigma({mem_ty, inputs->type()}), w.sigma({mem_ty, type}))->set("mapRedAff");
    auto ds_fun  = direct::op_cps2ds_dep(fun);
    auto call    = w.app(ds_fun, w.tuple({cur_mem_, inputs}));
    auto fun_arg = fun->var(0_n);
    auto [fun_mem, new_inputs] = fun_arg->projs<2>();
    auto cont                  = fun->var(1); // return continuation `Cn[mem, result_buf]`

    // Allocate the output buffer (folded metadata read off the result type); the accumulator carried through the
    // output loops is `{mem, buf}`.
    auto [or_, os, oT]    = Axm::isa<buffer::Buf>(type)->args<3>();
    auto [a_mem, out_buf] = buffer::op_alloc(or_, os, oT, fun_mem)->projs<2>();
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

    // Write-back continuation `Cn[mem, To]`: store the accumulated element into the output at `acc_out` coords.
    auto write_back              = mem::mut_con(To)->set("writeBack");
    auto [wb_in_mem, element_final] = write_back->vars<2>();
    DefVec wb_iters                 = out_iters;
    for (u64 j = 0; j < rr; ++j) wb_iters.push_back(w.call(core::conv::u, Sr->proj(nloops, ro + j), w.lit(w.type_i64(), 0)));
    auto write_coords = affine_map(acc_out, Ro, n, Sr, So, w.tuple(wb_iters)); // «Ro; Idx (So#k)»
    auto stored = buffer::op_write(or_, os, oT, wb_in_mem, wb_buf, fold_index(So, write_coords), element_final);
    write_back->app(true, cont, w.tuple({stored->proj(0), wb_buf}));

    // Reduction loops over the trailing Rr bounds of `Sr`; accumulator `{mem, element}`.
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

    // The full loop iteration vector `(o…, r…)`.
    DefVec iters_v = out_iters;
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    // Read one element from each input at its affine read coordinates, threading memory.
    auto cur = red_mem;
    DefVec input_elements(nis_nat);
    for (u64 i = 0; i < nis_nat; ++i) {
        auto in_buf        = new_inputs->proj(nis_nat, i);
        auto coords        = affine_map(accs->proj(nis_nat, i), Ris->proj(nis_nat, i), n, Sr, Sis->proj(nis_nat, i), iters);
        auto [ir, is_, iT] = Axm::isa<buffer::Buf>(in_buf->type())->args<3>(); // actual (folded) input buffer metadata
        auto rd            = buffer::op_read(ir, is_, iT, cur, in_buf, fold_index(Sis->proj(nis_nat, i), coords));
        cur                = rd->proj(0);
        input_elements[i]  = rd->proj(1);
    }

    // The combiner is pure (`Fn [To, «nis; Tis»] → To`); pair its result with the threaded memory and yield.
    auto after_comb = w.mut_con(To)->set("afterComb");
    after_comb->app(true, cont, w.tuple({cur, after_comb->var(0_n)}));
    current_mut->app(true, comb, w.tuple({w.tuple({element_acc, w.tuple(input_elements)}), after_comb}));

    cur_mem_ = call->proj(0);
    return call->proj(1);
}

} // namespace mim::plug::tensor::phase
