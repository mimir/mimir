#include "mim/plug/gpu/phase/lower_map_reduce.h"

#include <algorithm>

#include <fe/log.h>
#include <fe/vector.h>

#include <mim/driver.h>
#include <mim/lam.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/btensor/btensor.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

namespace mim::plug::gpu::phase {

namespace {

using fe::Vector;

/// Whether an explicit, user-written `%gpu.init` is reachable from `def`.
bool contains_gpu_init(const Def* def, DefSet& seen) {
    if (auto [_, ins] = seen.emplace(def); !ins) return false;
    if (Axm::isa<gpu::init>(def)) return true;
    for (auto d : def->deps())
        if (contains_gpu_init(d, seen)) return true;
    return false;
}

/// Mirrors `btensor::phase::LowerMapReduce`'s helper of the same name: a counting `%affine.For` loop body
std::pair<Lam*, const Def*> counting_for(const Def* bound, const Def* acc, const Def* exit, Sym name) {
    auto& w       = bound->world();
    auto acc_ty   = acc->type();
    auto body     = w.mut_con({/* iter */ w.type_i64(), /* acc */ acc_ty, /* return */ w.cn(acc_ty)})->set(name);
    auto for_loop = w.call<affine::For>(body, exit, Defs{w.lit_i64(0), bound, w.lit_i64(1), acc});
    return {body, for_loop};
}

/// Mirrors `btensor::phase::LowerMapReduce`'s `affine_map` lambda
std::pair<const Def*, const Def*>
affine_map(const Def* f, const Def* m, const Def* n, const Def* sin, const Def* sout, const Def* idxs, const Def* mem) {
    auto& w = mem->world();
    auto a  = w.app(w.annex<affine::map>(), w.tuple({m, n}));
    a       = w.app(a, w.tuple({sin, sout}));
    a       = w.app(a, f);
    a       = w.app(a, idxs);
    a       = w.app(a, mem->type()->as<App>()->arg());
    return w.app(a, mem)->projs<2>();
}

const Def* fold_index(const Def* shape, const Def* idx) {
    auto& w = shape->world();
    auto r  = shape->num_projs();
    DefVec out;
    bool dropped = false;
    for (size_t i = 0; i != r; ++i)
        if (auto l = Lit::isa<nat_t>(shape->proj(r, i)); l && *l == 1)
            dropped = true;
        else
            out.push_back(idx->proj(r, i));
    // Without dropped axes the tuple below would just eta-reduce back to `idx` — but only after
    // World::tuple's pack normalization has alpha-compared the projections, which walks `idx`'s whole
    // (mem-threaded, Var-dependent) coordinate chain per elem pair — exponentially. Return `idx` directly.
    if (!dropped) return idx;
    return w.tuple(out);
}

/// Chained `%mem.lea` over a coordinate tuple.
const Def* op_lea_tuple(const Def* ptr, const Def* tuple) {
    auto n       = tuple->num_projs();
    auto element = ptr;
    for (size_t i = 0; i != n; ++i)
        element = mem::op_lea(element, tuple->proj(n, i));
    return element;
}

/// Scalarize may flatten an escaped `comb`/`post` from `Cn [[mem, T, ins], Cn ret]` to `Cn [mem, T, ins, Cn ret]`.
Lam* rebuild_lam_global_mem(Lam* lam, const Def* Tout, Sym name) {
    auto& w        = lam->world();
    auto global_ty = w.annex<gpu::GlobalM>();

    Lam* new_lam;
    if (lam->num_vars() == 2) {
        auto [_, Tin, extra_ty] = lam->var(0)->type()->projs<3>();
        new_lam = w.mut_con(Defs{w.sigma({global_ty, Tin, extra_ty}), w.cn({global_ty, Tout})})->set(name);
    } else {
        auto Tin      = lam->var(1)->type();
        auto extra_ty = lam->var(2)->type();
        new_lam       = w.mut_con(Defs{global_ty, Tin, extra_ty, w.cn({global_ty, Tout})})->set(name);
    }
    new_lam->set(true, lam->reduce_body(new_lam->var()));
    return new_lam;
}

/// Mirrors `btensor::phase::LowerMapReduce`'s helper of the same name.
void apply_cps(World& w, Lam* mut, const Def* f, DefVec parts, const Def* k) {
    auto dom = f->type()->as<Pi>()->dom();
    if (dom->num_projs() == parts.size() + 1) {
        parts.emplace_back(k);
        mut->app(true, f, w.tuple(parts));
    } else {
        mut->app(true, f, w.tuple({w.tuple(parts), k}));
    }
}

Vector<nat_t> row_major_strides(const Vector<nat_t>& dims) {
    Vector<nat_t> strides(dims.size());
    nat_t acc = 1;
    for (auto i = dims.size(); i-- != 0;) {
        strides[i] = acc;
        acc *= dims[i];
    }
    return strides;
}

std::pair<const Def*, DefVec> unflatten_index(World& w, const Def* flat, const Vector<nat_t>& dims, const Def* mem) {
    auto strides = row_major_strides(dims);
    DefVec coords(dims.size());
    for (size_t d = 0; d != dims.size(); ++d) {
        auto [m1, q] = w.call(core::div::udiv, Defs{mem, w.tuple({flat, w.lit_i64(strides[d])})})->projs<2>();
        auto [m2, r] = w.call(core::div::urem, Defs{m1, w.tuple({q, w.lit_i64(dims[d])})})->projs<2>();
        mem          = m2;
        coords[d]    = w.call(core::conv::u, w.lit_nat(dims[d]), r);
    }
    return {mem, coords};
}

struct InputDesc {
    DefVec rs, ss, ts, accs;
};

InputDesc extract_input_desc(nat_t n, const Def* Rs, const Def* Ss, const Def* Ts, const Def* accs) {
    InputDesc desc{DefVec(n), DefVec(n), DefVec(n), DefVec(n)};
    for (nat_t i = 0; i != n; ++i) {
        desc.rs[i]   = Rs->proj(n, i);
        desc.ss[i]   = Ss->proj(n, i);
        desc.ts[i]   = Ts->proj(n, i);
        desc.accs[i] = accs->proj(n, i);
    }
    return desc;
}

struct Inputs {
    const Def* mem;
    const Def* global;
    DefVec dptrs;
};

Inputs alloc_copy_inputs(World& w,
                         const Def* m0,
                         const Def* m1,
                         const DefVec& ris,
                         const DefVec& sis,
                         const DefVec& tis,
                         const Def* inputs) {
    DefVec dptrs(ris.size());
    for (size_t i = 0; i != ris.size(); ++i) {
        auto alloc_copy    = w.app(w.app(w.annex<gpu::buf_alloc_copy>(), {ris[i], sis[i], tis[i]}),
                                   {m0, m1, inputs->proj(ris.size(), i)});
        auto [m2, g2, ptr] = alloc_copy->projs<3>();
        m0                 = m2;
        m1                 = g2;
        dptrs[i]           = ptr;
    }
    return {m0, m1, dptrs};
}

std::pair<const Def*, const Def*> alloc_output(World& w, const Def* m1, const Def* elem_ty, const Def* So, nat_t ro) {
    auto arr_ty = elem_ty;
    for (auto d = ro; d-- != 0;)
        arr_ty = w.arr(So->proj(ro, d), arr_ty);
    return w.app(w.app(w.annex<gpu::alloc>(gpu::alloc::block), arr_ty), m1)->projs<2>();
}

struct Grid {
    nat_t n_groups, n_items, total;
};

Grid grid_layout(const Vector<nat_t>& out_dims) {
    nat_t total = 1;
    for (auto d : out_dims)
        total *= d;
    nat_t n_items  = std::min<nat_t>(total, 1024);
    nat_t n_groups = (total + n_items - 1) / n_items;
    return {n_groups, n_items, total};
}

/// Per-input state for `build_kernel`: `InputDesc`'s shapes/access-functions plus `alloc_copy_inputs`'s pointers.
struct Mapped {
    DefVec rs, ss, dptrs, accs;
    nat_t n() const { return dptrs.size(); }
};

/// Builds the kernel: one thread per output point, reducing sequentially over the `rr` reduction dims.
Lam* build_kernel(World& w,
                  const Def* Ro,
                  nat_t rr,
                  const Vector<nat_t>& out_dims,
                  const Def* Sr,
                  const Def* So,
                  const Mapped& ins,
                  const Def* To,
                  const Def* acc_out,
                  const Def* init,
                  Lam* global_comb,
                  const Mapped& post_ins,
                  Lam* global_post,
                  const Def* Tp,
                  const Def* out_dptr,
                  const Grid& grid) {
    auto nis        = ins.n();
    auto nps        = post_ins.n();
    auto ro         = out_dims.size();
    auto nloops_nat = ro + rr;
    auto n          = w.lit_nat(nloops_nat);

    auto global_ty = w.annex<gpu::GlobalM>();
    auto shared_ty = w.annex<gpu::SharedM>();
    auto const_ty  = w.annex<gpu::ConstM>();
    auto local_ty  = w.annex<gpu::LocalM>();

    DefVec arg_tys(nis + nps + 1);
    for (size_t i = 0; i != nis; ++i)
        arg_tys[i] = ins.dptrs[i]->type();
    for (size_t j = 0; j != nps; ++j)
        arg_tys[nis + j] = post_ins.dptrs[j]->type();
    arg_tys[nis + nps] = out_dptr->type();

    auto kernel
        = w.mut_con(Defs{global_ty, shared_ty, const_ty, local_ty, w.type_idx(grid.n_groups), w.type_idx(grid.n_items),
                         w.sigma(Defs{}), w.sigma(arg_tys), w.cn({global_ty, shared_ty, const_ty, local_ty})})
              ->set("mapReduceKernel");
    auto [k_global, k_shared, k_const, k_local, group_id, item_id, k_shared_ptrs, k_args, k_ret] = kernel->vars<9>();

    DefVec k_dptrs(nis);
    for (size_t i = 0; i != nis; ++i)
        k_dptrs[i] = k_args->proj(nis + nps + 1, i);
    DefVec k_post_dptrs(nps);
    for (size_t j = 0; j != nps; ++j)
        k_post_dptrs[j] = k_args->proj(nis + nps + 1, nis + j);
    auto k_out_dptr = k_args->proj(nis + nps + 1, nis + nps);

    auto group_i64 = grid.n_groups == 1 ? w.lit_i64(0) : w.call(core::conv::u, w.lit_nat_0(), group_id);
    auto item_i64  = grid.n_items == 1 ? w.lit_i64(0) : w.call(core::conv::u, w.lit_nat_0(), item_id);
    auto flat
        = w.call(core::wrap::add, core::Mode::none,
                 Defs{w.call(core::wrap::mul, core::Mode::none, Defs{group_i64, w.lit_i64(grid.n_items)}), item_i64});

    auto in_range     = w.call(core::icmp::ul, w.tuple({flat, w.lit_i64(grid.total)}));
    auto early_return = w.mut_con(w.sigma(Defs{}))->set("outOfRange");
    early_return->app(true, k_ret, w.tuple({k_global, k_shared, k_const, k_local}));
    auto body = w.mut_con(w.sigma(Defs{}))->set("inRange");
    kernel->set(true, w.app(w.extract(w.tuple({early_return, body}), in_range), w.tuple()));

    auto write_back           = w.mut_con(Defs{global_ty, To})->set("writeBack");
    auto [wb_mem, acc_final]  = write_back->vars<2>();
    auto [wb_mem2, wb_coords] = unflatten_index(w, flat, out_dims, wb_mem);
    DefVec wb_idx             = wb_coords;
    for (size_t j = 0; j != rr; ++j)
        wb_idx.push_back(w.call(core::conv::u, Sr->proj(nloops_nat, ro + j), w.lit_i64(0)));
    auto [wc_mem, write_coords] = affine_map(acc_out, Ro, n, Sr, So, w.tuple(wb_idx), wb_mem2);

    auto pcur = wc_mem;
    DefVec post_elems(nps);
    for (size_t j = 0; j != nps; ++j) {
        auto [pc_mem, pcoords]
            = affine_map(post_ins.accs[j], post_ins.rs[j], Ro, So, post_ins.ss[j], write_coords, pcur);
        pcur = pc_mem;
        auto [rd_mem, rd_val]
            = w.call<mem::load>(Defs{pcur, op_lea_tuple(k_post_dptrs[j], fold_index(post_ins.ss[j], pcoords))})
                  ->projs<2>();
        pcur          = rd_mem;
        post_elems[j] = rd_val;
    }

    auto after_post            = w.mut_con(Defs{global_ty, Tp})->set("afterPost");
    auto [post_mem, elem_post] = after_post->vars<2>();
    auto final_mem
        = w.call<mem::store>(Defs{post_mem, op_lea_tuple(k_out_dptr, fold_index(So, write_coords)), elem_post});
    after_post->app(true, k_ret, w.tuple({final_mem, k_shared, k_const, k_local}));
    apply_cps(w, write_back, global_post, {pcur, acc_final, w.tuple(post_elems)}, after_post);

    const Def* acc   = w.tuple({k_global, init});
    const Def* cont  = write_back;
    Lam* current_mut = body;
    DefVec red_iters;
    red_iters.reserve(rr);
    for (size_t j = 0; j != rr; ++j) {
        auto dim                    = Sr->proj(nloops_nat, ro + j);
        auto bound                  = w.call<core::bitcast>(w.type_i64(), dim);
        auto [rbody, for_call]      = counting_for(bound, acc, cont, w.sym("forRed_" + std::to_string(j)));
        auto [iter, new_acc, yield] = rbody->vars<3>();
        cont                        = yield;
        red_iters.push_back(w.call(core::conv::u, dim, iter));
        acc = new_acc;
        current_mut->set(true, for_call);
        current_mut = rbody;
    }
    auto [red_mem, elem_acc] = acc->projs<2>();

    auto [body_mem, body_coords] = unflatten_index(w, flat, out_dims, red_mem);
    DefVec iters_v               = body_coords;
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    auto cur = body_mem;
    DefVec input_elems(nis);
    for (size_t i = 0; i != nis; ++i) {
        auto [mc_mem, coords] = affine_map(ins.accs[i], ins.rs[i], n, Sr, ins.ss[i], iters, cur);
        cur                   = mc_mem;
        auto [rd_mem, rd_val]
            = w.call<mem::load>(Defs{cur, op_lea_tuple(k_dptrs[i], fold_index(ins.ss[i], coords))})->projs<2>();
        cur            = rd_mem;
        input_elems[i] = rd_val;
    }

    apply_cps(w, current_mut, global_comb, {cur, elem_acc, w.tuple(input_elems)}, cont);

    return kernel;
}

Lam* build_teardown(World& w,
                    const Def* Ro,
                    const Def* So,
                    const Def* Tp,
                    const DefVec& dptrs,
                    const DefVec& post_dptrs,
                    const Def* out_dptr,
                    const Def* cont) {
    auto global_ty = w.annex<gpu::GlobalM>();
    auto const_ty  = w.annex<gpu::ConstM>();
    auto mem_ty    = w.call<mem::M>(0);

    auto after_launch                        = w.mut_con(Defs{mem_ty, global_ty, const_ty})->set("afterLaunch");
    auto [post_mem, post_global, post_const] = after_launch->vars<3>();

    auto [alloc_mem, host_buf] = buffer::op_alloc(Ro, So, Tp, post_mem)->projs<2>();
    auto copy_back
        = w.app(w.app(w.annex<gpu::buf_copy_to_host>(), {Ro, So, Tp}), {alloc_mem, post_global, out_dptr, host_buf});
    auto [cb_mem, cb_global] = copy_back->projs<2>();

    auto cur_global = cb_global;
    for (auto dptr : dptrs)
        cur_global = w.call(gpu::free::block, w.tuple({cur_global, dptr}));
    for (auto dptr : post_dptrs)
        cur_global = w.call(gpu::free::block, w.tuple({cur_global, dptr}));
    cur_global = w.call(gpu::free::block, w.tuple({cur_global, out_dptr}));

    auto final_mem = w.app(w.annex<gpu::auto_deinit>(), w.tuple({cb_mem, cur_global, post_const}));
    after_launch->app(true, cont, w.tuple({final_mem, host_buf}));
    return after_launch;
}

} // namespace

void LowerMapReduce::start() {
    DefSet seen;
    auto has_gpu_init
        = std::ranges::any_of(old_world().roots(), [&](auto def) { return contains_gpu_init(def, seen); });
    if (has_gpu_init) {
        log().w("not lowering any map-reduce operations to GPU: the program already contains an explicit `%gpu.init`");
        return;
    }
    Super::start();
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (Axm::isa<btensor::map_reduce_post>(app)) return lower_map_reduce_post(app);
    return Super::rewrite_imm_App(app);
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    if (is_bootstrapping()) return Super::rewrite_imm_App(app);

    auto& w = new_world();
    auto c  = rewrite(app->callee())->as<App>();

    auto [nis_nps, meta, shapes, in_tys, comb_init, acc_out, accs_all] = c->uncurry_args<7>();
    auto [nis, nps]                                                    = nis_nps->projs<2>();
    auto [To, Tp, Ro, Rn, sched_ty]                                    = meta->projs<5>();
    auto [So, Sr, sched]                                               = shapes->projs<3>();
    auto [Tis, Ris, Sis, Tps, Rps, Sps]                                = in_tys->projs<6>();
    auto [comb, init, post]                                            = comb_init->projs<3>();
    auto [accs, post_accs]                                             = accs_all->projs<2>();
    auto result_ty                                                     = rewrite(app->type());

    auto nis_l = Lit::isa<nat_t>(nis);
    auto nps_l = Lit::isa<nat_t>(nps);
    auto ro_l  = Lit::isa<nat_t>(Ro);
    auto rn_l  = Lit::isa<nat_t>(Rn);
    if (!nis_l || !nps_l || !ro_l || !rn_l || *rn_l < *ro_l) {
        log().w("{} doesn't have lowering-time known rank counts (nis/nps/Ro/Rn)", app);
        return Super::rewrite_imm_App(app);
    }
    auto nis_n = *nis_l;
    auto nps_n = *nps_l;
    auto ro    = *ro_l;
    auto rr    = *rn_l - *ro_l;

    Vector<nat_t> out_dims(ro);
    nat_t out_total = 1;
    for (nat_t d = 0; d != ro; ++d) {
        auto l = Lit::isa<nat_t>(Sr->proj(ro + rr, d));
        if (!l) {
            log().w("{} doesn't have a lowering-time known output (grid) shape", app);
            return Super::rewrite_imm_App(app);
        }
        out_dims[d] = *l;
        out_total *= *l;
    }
    if (out_total == 0) {
        log().w("{} has a zero-sized output, skipping GPU lowering", app);
        return Super::rewrite_imm_App(app);
    }

    auto comb_lam = comb->isa_mut<Lam>();
    auto post_lam = post->isa_mut<Lam>();
    if (!comb_lam || !post_lam) {
        log().w("{} doesn't have a lowering-time known combiner/epilogue", app);
        return Super::rewrite_imm_App(app);
    }

    auto mem_ty                                    = w.call<mem::M>(0);
    auto rewritten_arg                             = rewrite(app->arg());
    auto [_, rewritten_inputs, rewritten_post_ins] = rewritten_arg->projs<3>();
    auto fun  = w.mut_fun(w.sigma({mem_ty, rewritten_inputs->type(), rewritten_post_ins->type()}), result_ty)
                    ->set("mapReduceAffGpu");
    auto call = w.app(cps::op_cps2ds_dep(fun), rewritten_arg);
    auto [fun_mem, new_inputs, new_post_ins] = fun->var(0_n)->projs<3>();
    auto cont                                = fun->var(1);

    auto [h_mem, h_global, h_const] = w.app(w.annex<gpu::auto_init>(), fun_mem)->projs<3>();

    auto in_desc = extract_input_desc(nis_n, Ris, Sis, Tis, accs);
    auto inputs  = alloc_copy_inputs(w, h_mem, h_global, in_desc.rs, in_desc.ss, in_desc.ts, new_inputs);

    auto post_desc = extract_input_desc(nps_n, Rps, Sps, Tps, post_accs);
    auto post_inputs
        = alloc_copy_inputs(w, inputs.mem, inputs.global, post_desc.rs, post_desc.ss, post_desc.ts, new_post_ins);

    auto [out_global, out_dptr] = alloc_output(w, post_inputs.global, Tp, So, ro);

    auto global_comb = rebuild_lam_global_mem(comb_lam, To, w.sym("combGlobal"));
    auto global_post = rebuild_lam_global_mem(post_lam, Tp, w.sym("postGlobal"));

    auto grid = grid_layout(out_dims);

    auto kernel = build_kernel(w, Ro, rr, out_dims, Sr, So, Mapped{in_desc.rs, in_desc.ss, inputs.dptrs, in_desc.accs},
                               To, acc_out, init, global_comb,
                               Mapped{post_desc.rs, post_desc.ss, post_inputs.dptrs, post_desc.accs}, global_post, Tp,
                               out_dptr, grid);

    DefVec kernel_arg_tys(nis_n + nps_n + 1);
    for (nat_t i = 0; i != nis_n; ++i)
        kernel_arg_tys[i] = inputs.dptrs[i]->type();
    for (nat_t j = 0; j != nps_n; ++j)
        kernel_arg_tys[nis_n + j] = post_inputs.dptrs[j]->type();
    kernel_arg_tys[nis_n + nps_n] = out_dptr->type();

    auto launch = w.app(w.annex<gpu::launch>(), w.tuple({w.lit_nat(nis_n + nps_n + 1), w.tuple(kernel_arg_tys)}));
    launch = w.app(launch, w.tuple({w.lit_nat(grid.n_groups), w.lit_nat(grid.n_items), w.annex<gpu::default_stream>(),
                                    w.lit_ff(), w.tuple(Defs{})}));
    launch = w.app(launch, kernel);

    DefVec kernel_args = inputs.dptrs;
    kernel_args.insert(kernel_args.end(), post_inputs.dptrs.begin(), post_inputs.dptrs.end());
    kernel_args.push_back(out_dptr);
    launch = w.app(launch, w.tuple(kernel_args));

    auto after_launch = build_teardown(w, Ro, So, Tp, inputs.dptrs, post_inputs.dptrs, out_dptr, cont);
    auto launch_call  = w.app(launch, w.tuple({w.tuple({post_inputs.mem, out_global, h_const}), after_launch}));
    fun->set(true, launch_call);

    return call;
}

} // namespace mim::plug::gpu::phase
