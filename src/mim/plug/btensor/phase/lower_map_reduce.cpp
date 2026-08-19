#include "mim/plug/btensor/phase/lower_map_reduce.h"

#include <mim/axm.h>
#include <mim/def.h>
#include <mim/lam.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/btensor/btensor.h"

namespace mim::plug::btensor::phase {

namespace {

/// Drops, from the (unfolded) index `idx`, the components of size-1 dimensions of `shape`
/// (`%buffer.Buf` normalizes size-1 axes away, mirroring the folding of array types).
const Def* fold_index(const Def* shape, const Def* idx) {
    auto& w = shape->world();
    auto r  = shape->num_projs();
    DefVec out;
    bool dropped = false;
    for (size_t i = 0; i != r; ++i)
        if (auto l = Lit::isa<u64>(shape->proj(r, i)); l && *l == 1)
            dropped = true;
        else
            out.push_back(idx->proj(r, i));
    // Without dropped axes the tuple below would just eta-reduce back to `idx` — but only after
    // World::tuple's pack normalization has alpha-compared the projections, which walks `idx`'s whole
    // (mem-threaded, Var-dependent) coordinate chain per elem pair — exponentially. Return `idx` directly.
    if (!dropped) return idx;
    return w.tuple(out);
}

/// Builds a counting `affine.For` loop body carrying `acc` (a `{mem, …}` tuple).
std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

/// Pointwise scaffold shared by `pad`/`concat`: a `[mem, ins] → [mem, Buf]` fun (spliced via
/// `%cps.cps2ds`) that allocates the output buffer, loops over `s_out` carrying `{mem, buf}`, and writes
/// `compute`'s elem at the (identity) output coordinates.
/// `compute(iters, ins, mem)` receives the raw i64 loop counters, the fun's inputs var, and the current
/// mem; it returns `(mem', elem)`.
template<class Compute>
const Def* build_pointwise(World& w,
                           const Def* result_ty, // [%mem.M 0, %buffer.Buf (r, s_out, T)]
                           const Def* op_mem,
                           const Def* op_ins,
                           const Def* s_out,
                           u64 rn,
                           const std::string& name,
                           Compute&& compute) {
    auto mem_ty         = w.call<mem::M>(0);
    auto fun            = w.mut_fun(w.sigma({mem_ty, op_ins->type()}), result_ty)->set(name);
    auto call           = w.app(cps::op_cps2ds_dep(fun), w.tuple({op_mem, op_ins}));
    auto [fun_mem, ins] = fun->var(0_n)->projs<2>();
    auto cont           = fun->var(1);

    auto [obr, obs, obT]  = Axm::isa<buffer::Buf>(result_ty->proj(1))->args<3>();
    auto [a_mem, out_buf] = buffer::op_alloc(obr, obs, obT, fun_mem)->projs<2>();
    const Def* acc        = w.tuple({a_mem, out_buf});
    auto current_mut      = fun;

    DefVec iters; // raw i64 loop counters
    iters.reserve(rn);
    for (u64 d = 0; d < rn; ++d) {
        auto bound                  = w.call<core::bitcast>(w.type_i64(), s_out->proj(rn, d));
        auto [body, for_call]       = counting_for(bound, acc, cont, w.sym(name + "_" + std::to_string(d)));
        auto [iter, new_acc, yield] = body->vars<3>();
        cont                        = yield;
        iters.push_back(iter);
        acc = new_acc;
        current_mut->set(true, for_call);
        current_mut = body;
    }
    auto [loop_mem, loop_buf] = acc->projs<2>();

    std::pair<const Def*, const Def*> el = compute(iters, ins, loop_mem);
    auto [el_mem, elem]                  = el;

    DefVec wcoords(rn);
    for (u64 d = 0; d < rn; ++d)
        wcoords[d] = w.call(core::conv::u, s_out->proj(rn, d), iters[d]);
    auto [wr_mem, wr_buf]
        = buffer::op_write(obr, obs, obT, el_mem, loop_buf, fold_index(s_out, w.tuple(wcoords)), elem)->projs<2>();
    current_mut->app(true, cont, w.tuple({wr_mem, loop_buf}));
    return call;
}

} // namespace

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    if (Axm::isa<btensor::map_reduce>(app)) return lower_map_reduce(app);
    if (Axm::isa<btensor::broadcast>(app)) return lower_broadcast(app);
    if (Axm::isa<btensor::pad>(app)) return lower_pad(app);
    if (Axm::isa<btensor::concat>(app)) return lower_concat(app);
    if (Axm::isa<buffer::constant>(app)) return lower_buffer_constant(app);
    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerMapReduce::lower_buffer_constant(const App* app) {
    // `%buffer.constant (r, s, T) (mem, val)` fills every elem with `val`. Emit a fill loop (via the same
    // pointwise scaffold as pad/concat) so the store never materializes as one giant literal array. A
    // non-literal rank has no static loop nest, so leave it for `%buffer.lower_ptr`'s monolithic fallback.
    auto [r, s, T] = app->callee()->as<App>()->args<3>();
    auto s_out     = rewrite(s);
    auto rn        = Lit::isa<u64>(rewrite(r));
    if (!rn) return RWPhase::rewrite_imm_App(app);

    auto& w         = new_world();
    auto [mem, val] = rewrite(app->arg())->projs<2>();
    auto result_ty  = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (r, s, T)]
    // `compute` ignores the loop counters and writes the (loop-invariant) scalar `ins` everywhere.
    return build_pointwise(
        w, result_ty, mem, val, s_out, *rn, "constant_fill",
        [](const DefVec&, const Def* ins, const Def* m) -> std::pair<const Def*, const Def*> { return {m, ins}; });
}

const Def* LowerMapReduce::lower_map_reduce(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs] = c->uncurry_args<7>();
    auto [To, Ro, Rn]                                             = meta->projs<3>();
    auto [So, Sr]                                                 = shapes->projs<2>();
    auto [Tis, Ris, Sis]                                          = TisRisSis->projs<3>();
    auto [comb, init]                                             = comb_init->projs<2>();

    // The final argument is `[mem, is]`; the result is `[mem, Buf]`.
    auto [op_mem, op_is] = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (Ro, So, To)]

    auto nis_l = Lit::isa<u64>(nis);
    auto ro_l = Lit::isa<u64>(Ro), rn_l = Lit::isa<u64>(Rn);
    if (!nis_l || !ro_l || !rn_l || *rn_l < *ro_l) {
        WLOG("{} doesn't have lowering-time known rank counts (nis/Ro/Rn)", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto nis_nat = *nis_l;
    auto ro = *ro_l, rr = *rn_l - *ro_l;
    auto nloops = *rn_l;
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

    // `[mem, is] → [mem, Buf]`, spliced via %cps.cps2ds and applied to the op's (mem, is).
    auto fun                   = w.mut_fun(w.sigma({mem_ty, op_is->type()}), result_ty)->set("mapRedAff");
    auto call                  = w.app(cps::op_cps2ds_dep(fun), w.tuple({op_mem, op_is}));
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
    auto write_back              = mem::mut_con(To)->set("writeBack");
    auto [wb_in_mem, elem_final] = write_back->vars<2>();
    DefVec wb_iters              = out_iters;
    for (u64 j = 0; j < rr; ++j)
        wb_iters.push_back(w.call(core::conv::u, Sr->proj(nloops, ro + j), w.lit(w.type_i64(), 0)));
    auto [wc_mem, write_coords] = affine_map(acc_out, Ro, n, Sr, So, w.tuple(wb_iters), wb_in_mem);
    auto stored = buffer::op_write(obr, obs, obT, wc_mem, wb_buf, fold_index(So, write_coords), elem_final);
    write_back->app(true, cont, w.tuple({stored->proj(0), wb_buf}));

    // Reduction loops; accumulator `{mem, elem}`.
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
    auto [red_mem, elem_acc] = acc->projs<2>();

    DefVec iters_v = out_iters;
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    // Read one elem from each input at its affine coordinates, threading memory.
    auto cur = red_mem;
    DefVec input_elems(nis_nat);
    for (u64 i = 0; i < nis_nat; ++i) {
        auto in_buf = new_inputs->proj(nis_nat, i);
        auto [mc_mem, coords]
            = affine_map(accs->proj(nis_nat, i), Ris->proj(nis_nat, i), n, Sr, Sis->proj(nis_nat, i), iters, cur);
        cur                = mc_mem;
        auto [ir, is_, iT] = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
        auto [rd_mem, rd_val]
            = buffer::op_read(ir, is_, iT, cur, in_buf, fold_index(Sis->proj(nis_nat, i), coords))->projs<2>();
        cur            = rd_mem;
        input_elems[i] = rd_val;
    }

    // The combiner is mem-threaded (`Fn [mem, To, «nis; Tis»] → [mem, To]`): call it directly, its result feeds `cont`.
    current_mut->app(true, comb, w.tuple({w.tuple({cur, elem_acc, w.tuple(input_elems)}), cont}));

    return call;
}

const Def* LowerMapReduce::lower_broadcast(const App* app) {
    auto& w              = new_world();
    auto callee          = app->callee()->as<App>(); // (broadcast {impl}) (s_in, s_out)
    auto [s_in, s_out]   = rewrite(callee->arg())->projs<2>();
    auto [op_mem, input] = rewrite(app->arg())->projs<2>();
    auto result_ty       = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (ro, so, T)]

    auto r_nat = s_out->num_projs();

    auto mem_ty            = w.call<mem::M>(0);
    auto fun               = w.mut_fun(w.sigma({mem_ty, input->type()}), result_ty)->set("broadcast");
    auto call              = w.app(cps::op_cps2ds_dep(fun), w.tuple({op_mem, input}));
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

const Def* LowerMapReduce::lower_pad(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    // callee: pad {T, r} [s_in] [s_out, mode, lo, hi]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [Tr, s_in, params]     = c->uncurry_args<3>();
    auto [s_out, mode, lo, hi]  = params->projs<4>();
    auto [op_mem, input, value] = rewrite(app->arg())->projs<3>();
    auto result_ty              = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (r, s_out, T)]

    auto r_l    = Lit::isa<u64>(Tr->proj(2, 1));
    auto mode_l = Lit::isa<u64>(mode);
    if (!r_l || !mode_l) {
        WLOG("{} doesn't have a lowering-time known rank/mode", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto rn       = *r_l;
    auto mode_nat = *mode_l;
    auto i64      = w.type_i64();

    // select(cond, t, f) == `(f, t)#cond` (cf. %core.select); cond : Bool.
    auto sel = [&](const Def* cond, const Def* t, const Def* f) { return w.extract(w.tuple({f, t}), cond); };

    auto compute = [&](const DefVec& iters, const Def* ins, const Def* mem) -> std::pair<const Def*, const Def*> {
        auto [in_buf, fill]  = ins->projs<2>();
        auto [ibr, ibs, ibT] = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
        DefVec clamped(rn); // per-axis read index, kept in range, as `Idx (s_in#d)`
        DefVec valid;       // per-axis in-bounds flag (constant mode only)
        for (u64 d = 0; d < rn; ++d) {
            auto lo_d  = w.call<core::bitcast>(i64, lo->proj(rn, d));
            auto sin_d = w.call<core::bitcast>(i64, s_in->proj(rn, d));
            auto in_d  = w.call(core::wrap::sub, core::Mode::none, Defs{iters[d], lo_d}); // o#d − lo#d
            const Def* idx_i64;
            if (mode_nat == 0) { // constant: a single unsigned `<` covers both bounds (underflow wraps high)
                auto v_d = w.call(core::icmp::ul, w.tuple({in_d, sin_d}));
                valid.push_back(v_d);
                idx_i64 = sel(v_d, in_d, w.lit_i64(0));
            } else { // replicate: clamp the read to the nearest edge [0, s_in#d − 1]
                auto sin_m1 = w.call(core::wrap::sub, core::Mode::none, Defs{sin_d, w.lit_i64(1)});
                idx_i64     = w.call(core::extrema::smax,
                                     w.tuple({w.lit_i64(0), w.call(core::extrema::smin, w.tuple({in_d, sin_m1}))}));
            }
            clamped[d] = w.call(core::conv::u, s_in->proj(rn, d), idx_i64);
        }
        auto [rd_mem, elem]
            = buffer::op_read(ibr, ibs, ibT, mem, in_buf, fold_index(s_in, w.tuple(clamped)))->projs<2>();
        if (mode_nat != 0) return {rd_mem, elem}; // replicate: always a (clamped) read
        auto all_valid = valid.empty() ? w.lit_tt() : valid[0];
        for (u64 d = 1; d < valid.size(); ++d)
            all_valid = w.call(core::bit2::and_, w.lit_nat(2), w.tuple({all_valid, valid[d]}));
        return {rd_mem, sel(all_valid, elem, fill)}; // constant: fill out-of-region cells with `value`
    };

    return build_pointwise(w, result_ty, op_mem, w.tuple({input, value}), s_out, rn, "pad", compute);
}

const Def* LowerMapReduce::lower_concat(const App* app) {
    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    // callee: concat {T, nis, r} [ax] {Sis} [s_out]. The shapes are the logical ones; buffer reads and
    // writes fold size-1 axes (the `Buf` handles are normalized), while the loops cover all logical dims.
    auto [TnisR, ax, Sis, s_out] = c->uncurry_args<4>();
    auto [T, nis, r]             = TnisR->projs<3>();
    auto [op_mem, op_is]         = rewrite(app->arg())->projs<2>();
    auto result_ty               = rewrite(app->type()); // [%mem.M 0, %buffer.Buf (r, s_out, T)]

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) {
        WLOG("{} doesn't have lowering-time known nis/rank/axis", app);
        return RWPhase::rewrite_imm_App(app);
    }
    auto nisn = *nis_l, rn = *r_l, axn = *ax_l;

    // Prefix offsets along `ax`: off#i = Σ_{j<i} Sis#i#ax (literal extents required).
    DefVec off(nisn);
    Vector<u64> ext(nisn);
    u64 acc_off = 0;
    for (u64 i = 0; i < nisn; ++i) {
        off[i]  = w.lit_i64(acc_off);
        auto ei = Lit::isa<u64>(Sis->proj(nisn, i)->proj(rn, axn));
        if (!ei) {
            WLOG("{} input {} has a non-literal extent along the concat axis", app, i);
            return RWPhase::rewrite_imm_App(app);
        }
        ext[i] = *ei;
        acc_off += *ei;
    }

    auto sel = [&](const Def* cond, const Def* t, const Def* f) { return w.extract(w.tuple({f, t}), cond); };

    auto compute = [&](const DefVec& iters, const Def* ins, const Def* mem) -> std::pair<const Def*, const Def*> {
        auto o_ax      = iters[axn];
        const Def* cur = mem;
        // Read input `i` at `iters`, but with the `ax` coordinate shifted by off#i and clamped into input `i`.
        auto read_i = [&](u64 i) -> const Def* {
            auto in_buf          = ins->proj(nisn, i);
            auto [ibr, ibs, ibT] = Axm::isa<buffer::Buf>(in_buf->type())->args<3>();
            auto Sis_i           = Sis->proj(nisn, i);
            auto e_i_m1          = w.lit_i64(ext[i] - 1);
            auto loc             = w.call(core::wrap::sub, core::Mode::none, Defs{o_ax, off[i]});
            auto clamp           = w.call(core::extrema::smax,
                                          w.tuple({w.lit_i64(0), w.call(core::extrema::smin, w.tuple({loc, e_i_m1}))}));
            DefVec coords(rn);
            for (u64 d = 0; d < rn; ++d) {
                auto idx_i64 = (d == axn) ? clamp : iters[d];
                coords[d]    = w.call(core::conv::u, Sis_i->proj(rn, d), idx_i64);
            }
            auto [rd_mem, rd_val]
                = buffer::op_read(ibr, ibs, ibT, cur, in_buf, fold_index(Sis_i, w.tuple(coords)))->projs<2>();
            cur = rd_mem;
            return rd_val;
        };
        // Select chain: the highest `i` with off#i ≤ o_ax owns the cell (offsets increase, later wins).
        auto result = read_i(0);
        for (u64 i = 1; i < nisn; ++i) {
            auto cond = w.call(core::icmp::uge, w.tuple({o_ax, off[i]}));
            result    = sel(cond, read_i(i), result);
        }
        return {cur, result};
    };

    return build_pointwise(w, result_ty, op_mem, op_is, s_out, rn, "concat", compute);
}

} // namespace mim::plug::btensor::phase
