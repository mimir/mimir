#include "mim/plug/tensor/phase/lower_map_reduce.h"

#include <mim/def.h>
#include <mim/lam.h>

#include <mim/util/types.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

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

static const Def* get_element_type(const Def* type, u64 r) {
    auto cur = type;
    for (u64 i = 0; i < r; ++i)
        if (auto seq = cur->isa<Seq>())
            cur = seq->body();
        else
            break;
    return cur;
}

static const Def* nested_extract(World& w, const Def* matrix, const Def* coords, const Def* shape, u64 r) {
    auto T = get_element_type(matrix->type(), r);
    return op_get(T, w.lit_nat(r), shape, matrix, coords);
}

static const Def*
nested_insert(World& w, const Def* matrix, const Def* coords, const Def* shape, u64 r, const Def* elem) {
    auto T = get_element_type(matrix->type(), r);
    return op_set(T, w.lit_nat(r), shape, matrix, coords, elem);
}

const Def* LowerMapReduce::lower_map_reduce(const App* app) {
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

    // Builds `%affine.map @(m, n) @(sin, sout) f idxs mem` and returns the result coordinates (dropping the returned
    // mem). The emitted `%affine.map` is lowered to %core arithmetic by the subsequent %affine.lower_index. We
    // invent a fresh `⊥ : %mem.M 0` for the mem operand here; real mem threading is wired up later by `add_mem`.
    auto mem0       = w.app(w.annex<mem::M>(), w.lit_nat(0));
    auto affine_map = [&](const Def* f, const Def* m, const Def* n, const Def* sin, const Def* sout, const Def* idxs) {
        auto a = w.app(w.annex<affine::map>(), w.tuple({m, n}));
        a      = w.app(a, w.tuple({sin, sout}));
        a      = w.app(a, f);
        a      = w.app(a, idxs);
        return w.app(a, w.bot(mem0))->proj(2, 1); // drop the returned mem at proj 0
    };

    try {
        auto fun    = w.mut_fun(inputs->type(), type)->set("mapRed");
        auto ds_fun = cps::op_cps2ds_dep(fun)->set("dsFun");
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
        write_back->app(true, cont, nested_insert(w, wb_matrix, write_coords, So, ro, element_final));

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
            auto sis_i        = Sis->proj(nis_nat, i);
            auto coords       = affine_map(accs->proj(nis_nat, i), Ris->proj(nis_nat, i), n, Sr, sis_i, iters);
            input_elements[i] = nested_extract(w, input_matrix, coords, sis_i, ris_nat[i]);
        }

        comb->set("comb");
        current_mut->app(true, comb, {w.tuple({element_acc, w.tuple(input_elements)}), cont});
        return call;
    } catch (const std::exception& e) { fe::throwf("error during lowering map_reduce: {}", e.what()); }
}

const Def* LowerMapReduce::build_pointwise(const Def* inputs,
                                           const Def* type,
                                           const Def* So,
                                           u64 ro,
                                           std::function<const Def*(const DefVec&, const Def*)> compute) {
    auto& w = new_world();

    auto fun    = w.mut_fun(inputs->type(), type)->set("pointwise");
    auto ds_fun = cps::op_cps2ds_dep(fun)->set("dsFun");
    auto call   = w.app(ds_fun, inputs)->set("call");

    auto new_inputs = fun->var(0)->set("is");

    // Output loops over `So`, collecting the raw i64 iteration indices for `compute`.
    auto cont        = fun->var(1);
    auto acc         = w.bot(cont->type()->as<Pi>()->dom());
    auto current_mut = fun;
    DefVec out_iters; // raw i64 loop counters
    out_iters.reserve(ro);
    for (u64 i = 0; i < ro; ++i) {
        auto dim                    = So->proj(ro, i);
        auto bound                  = w.call<core::bitcast>(w.type_i64(), dim);
        auto [body, for_call]       = counting_for(bound, acc, cont, w.sym("forOut_" + std::to_string(i)));
        auto [iter, new_acc, yield] = body->vars<3>();
        cont                        = yield;
        out_iters.push_back(iter);
        acc = new_acc;
        current_mut->set(true, for_call);
        current_mut = body;
    }
    auto wb_matrix = acc;

    // Write the computed element at the (identity) output coordinates; convert the i64 counters to `Idx (So#k)`.
    DefVec write_coords(ro);
    for (u64 i = 0; i < ro; ++i)
        write_coords[i] = w.call(core::conv::u, So->proj(ro, i), out_iters[i]);
    auto element = compute(out_iters, new_inputs);
    current_mut->app(true, cont, nested_insert(w, wb_matrix, w.tuple(write_coords), So, ro, element));
    return call;
}

const Def* LowerMapReduce::lower_pad(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg()); // (input, value)
    auto type = rewrite(app->type());

    // callee: pad {T, r} [s_in] [mode, lo, hi]
    auto [Tr, s_in, params] = c->uncurry_args<3>();
    auto [T, r]             = Tr->projs<2>();
    auto [mode, lo, hi]     = params->projs<3>();

    auto r_l    = Lit::isa<u64>(r);
    auto mode_l = Lit::isa<u64>(mode);
    if (!r_l || !mode_l) {
        WLOG("{} doesn't have a lowering-time known rank/mode", app);
        return nullptr;
    }
    auto rn       = *r_l;
    auto mode_nat = *mode_l;
    auto i64      = w.type_i64();

    // Deduce the output shape: s_out#d = lo#d + s_in#d + hi#d.
    DefVec so(rn);
    auto inner_type = type;
    for (u64 d = 0; d < rn; ++d) {
        auto inner_type_seq = inner_type->as<Seq>();
        so[d]               = inner_type_seq->arity();
        inner_type          = inner_type_seq->body();
    }
    auto s_out = w.tuple(so);

    // select(cond, t, f) == `(f, t)#cond` (cf. %core.select); cond : Bool.
    auto sel = [&](const Def* cond, const Def* t, const Def* f) { return w.extract(w.tuple({f, t}), cond); };

    auto compute = [&](const DefVec& out_iters, const Def* new_inputs) -> const Def* {
        auto [input, value] = new_inputs->projs<2>();
        DefVec clamped(rn); // per-axis read index, kept in range, as `Idx (s_in#d)`
        DefVec valid;       // per-axis in-bounds flag (constant mode only)
        for (u64 d = 0; d < rn; ++d) {
            auto lo_d  = w.call<core::bitcast>(i64, lo->proj(rn, d));
            auto sin_d = w.call<core::bitcast>(i64, s_in->proj(rn, d));
            auto in_d  = w.call(core::wrap::sub, core::Mode::none, Defs{out_iters[d], lo_d}); // o#d − lo#d
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
        auto elem = nested_extract(w, input, w.tuple(clamped), s_in, rn);
        if (mode_nat != 0) return elem; // replicate: always a (clamped) read
        auto all_valid = valid.empty() ? w.lit_tt() : valid[0];
        for (u64 d = 1; d < valid.size(); ++d)
            all_valid = w.call(core::bit2::and_, w.lit_nat(2), w.tuple({all_valid, valid[d]}));
        return sel(all_valid, elem, value); // constant: fill out-of-region cells with `value`
    };

    return build_pointwise(args, type, s_out, rn, compute);
}

const Def* LowerMapReduce::lower_concat(const App* app) {
    auto& w   = new_world();
    auto c    = rewrite(app->callee())->as<App>();
    auto args = rewrite(app->arg()); // the `is` input tuple
    auto type = rewrite(app->type());

    // callee: concat {T, nis, r} [ax] {Sis}
    auto [TnisR, ax, Sis] = c->uncurry_args<3>();
    auto [T, nis, r]      = TnisR->projs<3>();

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) {
        WLOG("{} doesn't have lowering-time known nis/r/ax", app);
        return nullptr;
    }
    auto nisn = *nis_l, rn = *r_l, axn = *ax_l;
    auto i64 = w.type_i64();

    // Prefix offsets along `ax`: off#i = Σ_{j<i} Sis#j#ax (literal extents required).
    DefVec off(nisn);
    u64 acc_off = 0;
    for (u64 i = 0; i < nisn; ++i) {
        off[i]  = w.lit_i64(acc_off);
        auto ei = Lit::isa<u64>(Sis->proj(nisn, i)->proj(rn, axn));
        if (!ei) {
            WLOG("{} input {} has a non-literal extent along the concat axis", app, i);
            return nullptr;
        }
        acc_off += *ei;
    }

    // Deduce the output shape: the summed extent along `ax`, the shared extents elsewhere.
    DefVec so(rn);
    for (u64 d = 0; d < rn; ++d)
        so[d] = (d == axn) ? w.lit_nat(acc_off) : Sis->proj(nisn, 0)->proj(rn, d);
    auto s_out = w.tuple(so);

    auto sel = [&](const Def* cond, const Def* t, const Def* f) { return w.extract(w.tuple({f, t}), cond); };

    auto compute = [&](const DefVec& out_iters, const Def* new_inputs) -> const Def* {
        auto o_ax = out_iters[axn];
        // Read input `i` at `out_iters`, but with the `ax` coordinate shifted by off#i and clamped into input `i`.
        auto read_i = [&](u64 i) -> const Def* {
            auto Sis_i  = Sis->proj(nisn, i);
            auto e_i    = w.call<core::bitcast>(i64, Sis_i->proj(rn, axn));
            auto e_i_m1 = w.call(core::wrap::sub, core::Mode::none, Defs{e_i, w.lit_i64(1)});
            auto loc    = w.call(core::wrap::sub, core::Mode::none, Defs{o_ax, off[i]});
            auto clamp  = w.call(core::extrema::smax,
                                 w.tuple({w.lit_i64(0), w.call(core::extrema::smin, w.tuple({loc, e_i_m1}))}));
            DefVec coords(rn);
            for (u64 d = 0; d < rn; ++d) {
                auto idx_i64 = (d == axn) ? clamp : out_iters[d];
                coords[d]    = w.call(core::conv::u, Sis_i->proj(rn, d), idx_i64);
            }
            return nested_extract(w, new_inputs->proj(nisn, i), w.tuple(coords), Sis_i, rn);
        };
        // Select chain: the highest `i` with off#i ≤ o_ax owns the cell (offsets increase, later wins).
        auto result = read_i(0);
        for (u64 i = 1; i < nisn; ++i) {
            auto cond = w.call(core::icmp::uge, w.tuple({o_ax, off[i]}));
            result    = sel(cond, read_i(i), result);
        }
        return result;
    };

    return build_pointwise(args, type, s_out, rn, compute);
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (auto bc = Axm::isa<tensor::broadcast>(app)) {
        if (auto res = lower_broadcast(bc)) return res;
    } else if (auto mr = Axm::isa<tensor::map_reduce>(app)) {
        if (auto res = lower_map_reduce(mr)) return res;
    } else if (auto pad = Axm::isa<tensor::pad>(app)) {
        if (auto res = lower_pad(pad)) return res;
    } else if (auto cat = Axm::isa<tensor::concat>(app)) {
        if (auto res = lower_concat(cat)) return res;
    }
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::tensor::phase
