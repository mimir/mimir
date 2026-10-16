#include "mim/plug/gpu/phase/lower_map_reduce.h"

#include <algorithm>

#include <fe/log.h>
#include <fe/vector.h>

#include <mim/driver.h>
#include <mim/lam.h>
#include <mim/rewrite.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/btensor/btensor.h>
#include <mim/plug/buffer/buffer.h>
#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/gpu/gpu.h"

namespace mim::plug::gpu::phase {

namespace {

using fe::Vector;

/// Whether an explicit, user-written `gpu.init` is reachable from `def`.
bool contains_gpu_init(const Def* def, DefSet& seen) {
    if (auto [_, ins] = seen.emplace(def); !ins) return false;
    if (Axm::isa<gpu::init>(def)) return true;
    for (auto d : def->deps())
        if (contains_gpu_init(d, seen)) return true;
    return false;
}

/// Substitutes the var of @p old_lam and moves every memory in its Nest to `gpu.GlobalM`.
class GlobalMem : public Rewriter {
public:
    GlobalMem(Lam* old_lam, Lam* new_lam)
        : Rewriter(old_lam->world())
        , old_lam_(old_lam) {
        auto& w = world();
        map(old_lam->var(), new_lam->var());
        map(w.call<mem::M>(0), w.annex<gpu::GlobalM>());
    }

    const Def* rewrite(const Def* old_def) final {
        if (auto mut = old_def->isa_mut(); mut && !old_lam_->nests(mut)) return mut;
        return Rewriter::rewrite(old_def);
    }

private:
    Lam* old_lam_;
};

/// Scalarize may flatten an escaped `comb`/`post` from `Cn [[mem, T, ins], Cn ret]` to `Cn [mem, T, ins, Cn ret]`.
Lam* rebuild_lam_global_mem(Lam* lam, const Def* Tout, Sym name) {
    auto& w        = lam->world();
    auto global_ty = w.annex<gpu::GlobalM>();

    Lam* new_lam;
    auto nv = lam->num_vars();
    if (nv == 2) {
        auto [_, Tin, extra_ty] = lam->var(nv, 0)->type()->projs<3>();
        new_lam = w.mut_con(Defs{w.sigma({global_ty, Tin, extra_ty}), w.cn({global_ty, Tout})})->set(name);
    } else {
        auto Tin      = lam->var(nv, 1)->type();
        auto extra_ty = lam->var(nv, 2)->type();
        new_lam       = w.mut_con(Defs{global_ty, Tin, extra_ty, w.cn({global_ty, Tout})})->set(name);
    }
    new_lam->set(true, GlobalMem(lam, new_lam).rewrite(lam->body()));
    return new_lam;
}

/// The row-major coordinates of @p flat - an `I64`, i.e. an `Idx 0` - via `affine.delinearize`.
std::pair<const Def*, DefVec> unflatten_index(World& w, const Def* flat, const Vector<nat_t>& dims, const Def* mem) {
    auto m = dims.size();
    if (m == 0) return {mem, {}};

    auto s_out = w.tuple(DefVec(m, [&](size_t d) { return w.lit_nat(dims[d]); }));
    auto index = w.annex<affine::Index>();
    auto f     = w.mut_lam(index, w.arr(m, index))->set("unflatten");
    f->set(true, w.app(w.app(w.annex<affine::delinearize>(), w.lit_nat(m)), {f->var(), s_out}));

    auto [mem2, coords]
        = btensor::op_affine_map(f, w.lit_nat(m), w.lit_nat_1(), w.lit_nat_0(), s_out, flat, mem)->projs<2>();
    return {mem2, coords->projs(m)};
}

struct InputDesc {
    DefVec rs, ss, ts, maps;
};

InputDesc extract_input_desc(nat_t n, const Def* Rs, const Def* Ss, const Def* Ts, const Def* maps) {
    InputDesc desc{DefVec(n), DefVec(n), DefVec(n), DefVec(n)};
    for (nat_t i = 0; i != n; ++i) {
        desc.rs[i]   = Rs->proj(n, i);
        desc.ss[i]   = Ss->proj(n, i);
        desc.ts[i]   = Ts->proj(n, i);
        desc.maps[i] = maps->proj(n, i);
    }
    return desc;
}

struct Inputs {
    const Def* mem;
    const Def* global;
    DefVec dptrs;
};

Inputs alloc_copy_inputs(World& w, const Def* m0, const Def* m1, Defs ris, Defs sis, Defs tis, const Def* inputs) {
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

std::pair<const Def*, const Def*> alloc_output(World& w, const Def* m1, const Def* elem_ty, const Def* So) {
    return w.app(w.app(w.annex<gpu::alloc>(gpu::alloc::block), w.arr(So, elem_ty)), m1)->projs<2>();
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
    DefVec rs, ss, dptrs, maps;
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
                  const Def* map_out,
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

    auto in_range     = w.call(core::icmp::ul, Defs{flat, w.lit_i64(grid.total)});
    auto early_return = w.mut_con(w.sigma(Defs{}))->set("outOfRange");
    early_return->app(true, k_ret, Defs{k_global, k_shared, k_const, k_local});
    auto body = w.mut_con(w.sigma(Defs{}))->set("inRange");
    kernel->branch(true, in_range, body, early_return);

    auto write_back           = w.mut_con(Defs{global_ty, To})->set("writeBack");
    auto [wb_mem, acc_final]  = write_back->vars<2>();
    auto [wb_mem2, wb_coords] = unflatten_index(w, flat, out_dims, wb_mem);
    DefVec wb_idx             = wb_coords;
    for (size_t j = 0; j != rr; ++j)
        wb_idx.push_back(w.call(core::conv::u, Sr->proj(nloops_nat, ro + j), w.lit_i64(0)));
    auto [wc_mem, write_coords] = btensor::op_affine_map(map_out, Ro, n, Sr, So, w.tuple(wb_idx), wb_mem2)->projs<2>();

    auto pcur = wc_mem;
    DefVec post_elems(nps);
    for (size_t j = 0; j != nps; ++j) {
        auto [pc_mem, pcoords]
            = btensor::op_affine_map(post_ins.maps[j], post_ins.rs[j], Ro, So, post_ins.ss[j], write_coords, pcur)
                  ->projs<2>();
        pcur = pc_mem;
        auto [rd_mem, rd_val]
            = w.call<mem::load>(Defs{pcur, mem::op_lea_tuple(k_post_dptrs[j], *Shape(pcoords).fold(post_ins.ss[j]))})
                  ->projs<2>();
        pcur          = rd_mem;
        post_elems[j] = rd_val;
    }

    auto after_post            = w.mut_con(Defs{global_ty, Tp})->set("afterPost");
    auto [post_mem, elem_post] = after_post->vars<2>();
    auto final_mem
        = w.call<mem::store>(Defs{post_mem, mem::op_lea_tuple(k_out_dptr, *Shape(write_coords).fold(So)), elem_post});
    after_post->app(true, k_ret, Defs{final_mem, k_shared, k_const, k_local});
    btensor::app_cps(write_back, global_post, {pcur, acc_final, w.tuple(post_elems)}, after_post);

    const Def* acc           = w.tuple({k_global, init});
    const Def* cont          = write_back;
    Lam* current_mut         = body;
    auto sr                  = Sr->projs(nloops_nat);
    auto raw                 = affine::counting_nest(current_mut, cont, acc, Defs(sr).subspan(ro, rr), "forRed");
    auto red_iters           = btensor::to_idxs(w, Defs(sr).subspan(ro, rr), raw);
    auto [red_mem, elem_acc] = acc->projs<2>();

    auto [body_mem, body_coords] = unflatten_index(w, flat, out_dims, red_mem);
    DefVec iters_v               = body_coords;
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    auto cur = body_mem;
    DefVec input_elems(nis);
    for (size_t i = 0; i != nis; ++i) {
        auto [mc_mem, coords]
            = btensor::op_affine_map(ins.maps[i], ins.rs[i], n, Sr, ins.ss[i], iters, cur)->projs<2>();
        cur = mc_mem;
        auto [rd_mem, rd_val]
            = w.call<mem::load>(Defs{cur, mem::op_lea_tuple(k_dptrs[i], *Shape(coords).fold(ins.ss[i]))})->projs<2>();
        cur            = rd_mem;
        input_elems[i] = rd_val;
    }

    btensor::app_cps(current_mut, global_comb, {cur, elem_acc, w.tuple(input_elems)}, cont);

    return kernel;
}

Lam* build_teardown(World& w,
                    const Def* Ro,
                    const Def* So,
                    const Def* Tp,
                    Defs dptrs,
                    Defs post_dptrs,
                    const Def* out_dptr,
                    const Def* host_buf,
                    const Def* cont) {
    auto global_ty = w.annex<gpu::GlobalM>();
    auto const_ty  = w.annex<gpu::ConstM>();
    auto mem_ty    = w.call<mem::M>(0);

    auto after_launch                        = w.mut_con(Defs{mem_ty, global_ty, const_ty})->set("afterLaunch");
    auto [post_mem, post_global, post_const] = after_launch->vars<3>();

    auto copy_back
        = w.app(w.app(w.annex<gpu::buf_copy_to_host>(), {Ro, So, Tp}), {post_mem, post_global, out_dptr, host_buf});
    auto [cb_mem, cb_global] = copy_back->projs<2>();

    auto cur_global = cb_global;
    for (auto dptr : dptrs)
        cur_global = w.call(gpu::free::block, Defs{cur_global, dptr});
    for (auto dptr : post_dptrs)
        cur_global = w.call(gpu::free::block, Defs{cur_global, dptr});
    cur_global = w.call(gpu::free::block, Defs{cur_global, out_dptr});

    auto final_mem = w.app(w.annex<gpu::auto_deinit>(), Defs{cb_mem, cur_global, post_const});
    after_launch->app(true, cont, Defs{final_mem, host_buf});
    return after_launch;
}

} // namespace

void LowerMapReduce::start() {
    DefSet seen;
    auto has_gpu_init
        = std::ranges::any_of(old_world().externals().muts(), [&](auto def) { return contains_gpu_init(def, seen); });
    if (has_gpu_init) {
        log().w("not lowering any map-reduce operations to GPU: the program already contains an explicit `gpu.init`");
        return;
    }
    Super::start();
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (Axm::isa<btensor::map_reduce_post>(app)) return lower_map_reduce_post(app);
    return Super::rewrite_imm_App(app);
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    auto& w        = new_world();
    auto mr        = btensor::MapRedPost::of(rewrite(app->callee())->as<App>());
    auto Sr        = mr.Sr;
    auto result_ty = rewrite(app->type());

    auto ranks = mr.ranks();
    if (!ranks) {
        log().w("{} doesn't have lowering-time known rank counts (nis/nps/Ro/Rn)", app);
        return Super::rewrite_imm_App(app);
    }
    auto [nis_n, nps_n, ro, rr] = *ranks;

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

    auto comb_lam = mr.comb->isa_mut<Lam>();
    auto post_lam = mr.post->isa_mut<Lam>();
    if (!comb_lam || !post_lam) {
        log().w("{} doesn't have a lowering-time known combiner/epilogue", app);
        return Super::rewrite_imm_App(app);
    }

    auto [fun, call] = btensor::ds_splice(rewrite(app->arg()), result_ty, "mapReduceAffGpu");
    auto [fun_mem, host_buf, new_inputs, new_post_ins] = fun->var(2, 0)->projs<4>();
    auto cont                                          = fun->var(2, 1);

    auto [h_mem, h_global, h_const] = w.app(w.annex<gpu::auto_init>(), fun_mem)->projs<3>();

    auto in_desc = extract_input_desc(nis_n, mr.Ris, mr.Sis, mr.Tis, mr.maps);
    auto inputs  = alloc_copy_inputs(w, h_mem, h_global, in_desc.rs, in_desc.ss, in_desc.ts, new_inputs);

    auto post_desc = extract_input_desc(nps_n, mr.Rps, mr.Sps, mr.Tps, mr.post_maps);
    auto post_inputs
        = alloc_copy_inputs(w, inputs.mem, inputs.global, post_desc.rs, post_desc.ss, post_desc.ts, new_post_ins);

    auto [out_global, out_dptr] = alloc_output(w, post_inputs.global, mr.Tp, mr.So);

    auto global_comb = rebuild_lam_global_mem(comb_lam, mr.To, w.sym("combGlobal"));
    auto global_post = rebuild_lam_global_mem(post_lam, mr.Tp, w.sym("postGlobal"));

    auto grid = grid_layout(out_dims);

    auto kernel = build_kernel(w, mr.Ro, rr, out_dims, Sr, mr.So,
                               Mapped{in_desc.rs, in_desc.ss, inputs.dptrs, in_desc.maps}, mr.To, mr.map_out, mr.init,
                               global_comb, Mapped{post_desc.rs, post_desc.ss, post_inputs.dptrs, post_desc.maps},
                               global_post, mr.Tp, out_dptr, grid);

    DefVec kernel_args = inputs.dptrs;
    kernel_args.insert(kernel_args.end(), post_inputs.dptrs.begin(), post_inputs.dptrs.end());
    kernel_args.push_back(out_dptr);
    DefVec kernel_arg_tys(kernel_args.size(), [&](size_t i) { return kernel_args[i]->type(); });

    auto launch = w.app(w.annex<gpu::launch>(), Defs{w.lit_nat(nis_n + nps_n + 1), w.tuple(kernel_arg_tys)});
    launch      = w.app(launch, Defs{w.lit_nat(grid.n_groups), w.lit_nat(grid.n_items), w.annex<gpu::default_stream>(),
                                     w.lit_ff(), w.tuple()});
    launch      = w.app(launch, kernel);
    launch      = w.app(launch, kernel_args);

    auto after_launch
        = build_teardown(w, mr.Ro, mr.So, mr.Tp, inputs.dptrs, post_inputs.dptrs, out_dptr, host_buf, cont);
    auto launch_call = w.app(launch, Defs{w.tuple({post_inputs.mem, out_global, h_const}), after_launch});
    fun->set(true, launch_call);

    return call;
}

} // namespace mim::plug::gpu::phase
