#include "mim/plug/gpu/phase/lower_map_reduce.h"

#include <algorithm>
#include <functional>
#include <numeric>

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

using fe::Vector;

struct LowerMapReduce::Call {
    struct In {
        Call* producer;  ///< The call chained into this slot, or `nullptr` for a host input.
        const Def* host; ///< The old slot value.
        size_t host_idx = 0;
    };

    const App* app;
    btensor::MapRedPost mr;
    nat_t ro, rr;
    Vector<nat_t> out_dims;
    Lam *comb, *post;
    Vector<In> ins, posts;
};

struct LowerMapReduce::Session {
    Lam* cur; ///< The continuation the next launch is emitted into.
    const Def *mem, *global, *const_tok;
    const Def* hosts;
    size_t n_hosts;
    DefVec to_free;
};

namespace {

/// Whether an explicit, user-written `gpu.init` is reachable from `def`.
bool contains_gpu_init(const Def* def, DefSet& seen) {
    if (auto [_, ins] = seen.emplace(def); !ins) return false;
    if (Axm::isa<gpu::init>(def)) return true;
    for (auto d : def->deps())
        if (contains_gpu_init(d, seen)) return true;
    return false;
}

void count_uses(const Def* def, DefMap<nat_t>& uses, DefSet& seen) {
    if (auto [_, ins] = seen.emplace(def); !ins) return;
    for (auto d : def->deps()) {
        ++uses[d];
        count_uses(d, uses, seen);
    }
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

nat_t product(const Vector<nat_t>& dims) {
    return std::accumulate(dims.begin(), dims.end(), nat_t(1), std::multiplies<>());
}

const Def* add_i64(const Def* a, const Def* b) {
    return a->world().call(core::wrap::add, core::Mode::none, Defs{a, b});
}

const Def* mul_i64(const Def* a, nat_t b) {
    auto& w = a->world();
    return w.call(core::wrap::mul, core::Mode::none, Defs{a, w.lit_i64(b)});
}

/// The row-major coordinates of @p flat - an `I64`, i.e. an `Idx 0` - via `affine.delinearize`, which wraps per axis.
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

/// One input of the op: its rank, shape, element type and access map, plus the device pointer it is read from.
struct Slot {
    const Def *r, *s, *t, *map, *dptr;
};

using Slots = Vector<Slot>;

/// What a kernel builder needs to know about the op it implements.
struct Op {
    World& w;
    const btensor::MapRedPost& mr;
    nat_t ro, rr;
    const Vector<nat_t>& out_dims;
    Slots ins, posts;
    Lam *comb, *post; ///< On `gpu.GlobalM`.
    const Def* out_dptr;

    nat_t rn() const { return ro + rr; }
    const Def* n() const { return w.lit_nat(rn()); }
    const Def* extent(nat_t pos) const { return mr.Sr->proj(rn(), pos); }
    const Def* zero(nat_t pos) const { return w.call(core::conv::u, extent(pos), w.lit_i64(0)); }
};

struct Kernel {
    Lam* lam;
    const Def *global, *shared, *const_tok, *local, *group, *item, *shared_ptr, *ret;
    DefVec ins, posts;
    const Def* out;
    nat_t n_groups, n_items;
    const Def* shared_ty; ///< The dynamic shared memory's type, or `nullptr` without one.
};

Kernel mk_kernel(const Op& op, nat_t n_groups, nat_t n_items, const Def* shared_ty, std::string name) {
    auto& w  = op.w;
    auto nis = op.ins.size();
    auto nps = op.posts.size();

    auto global_ty = w.annex<gpu::GlobalM>();
    auto shared_m  = w.annex<gpu::SharedM>();
    auto const_ty  = w.annex<gpu::ConstM>();
    auto local_ty  = w.annex<gpu::LocalM>();

    DefVec arg_tys;
    for (auto& in : op.ins)
        arg_tys.emplace_back(in.dptr->type());
    for (auto& in : op.posts)
        arg_tys.emplace_back(in.dptr->type());
    arg_tys.emplace_back(op.out_dptr->type());

    auto shared_ptr_ty = shared_ty ? w.app(w.annex<gpu::SharedPtr>(), shared_ty) : w.sigma();
    auto lam = w.mut_con(Defs{global_ty, shared_m, const_ty, local_ty, w.type_idx(n_groups), w.type_idx(n_items),
                              shared_ptr_ty, w.sigma(arg_tys), w.cn({global_ty, shared_m, const_ty, local_ty})})
                   ->set(name);
    auto [global, shared, const_tok, local, group, item, shared_ptr, args, ret] = lam->vars<9>();

    auto n = nis + nps + 1;
    Kernel k{lam,      global,     shared,   const_tok, local, group,
             item,     shared_ptr, ret,      {},        {},    args->proj(n, nis + nps),
             n_groups, n_items,    shared_ty};
    for (size_t i = 0; i != nis; ++i)
        k.ins.emplace_back(args->proj(n, i));
    for (size_t j = 0; j != nps; ++j)
        k.posts.emplace_back(args->proj(n, nis + j));
    return k;
}

const Def* group_i64(const Kernel& k) {
    auto& w = k.lam->world();
    return k.n_groups == 1 ? w.lit_i64(0) : w.call(core::conv::u, w.lit_nat_0(), k.group);
}

const Def* item_i64(const Kernel& k) {
    auto& w = k.lam->world();
    return k.n_items == 1 ? w.lit_i64(0) : w.call(core::conv::u, w.lit_nat_0(), k.item);
}

/// Reads @p slot through @p ptr at the indices @p idxs of the rank-`m_in` space `s_in`; @returns `{mem', elem}`.
std::pair<const Def*, const Def*>
load_at(const Slot& slot, const Def* ptr, const Def* m_in, const Def* s_in, const Def* idxs, const Def* mem) {
    auto& w          = mem->world();
    auto [m, coords] = btensor::op_affine_map(slot.map, slot.r, m_in, s_in, slot.s, idxs, mem)->projs<2>();
    return w.call<mem::load>(Defs{m, mem::op_lea_tuple(ptr, *Shape(coords).fold(slot.s))})->projs<2>();
}

/// Fills @p at with the epilogue: applies `post` to @p acc at the output cell of the parallel coordinates @p par,
/// stores the result and returns from the kernel.
void emit_epilogue(const Op& op,
                   const Kernel& k,
                   Lam* at,
                   const Def* global,
                   const Def* shared,
                   const Def* acc,
                   const DefVec& par) {
    auto& w  = op.w;
    auto& mr = op.mr;

    DefVec idx = par;
    for (nat_t j = 0; j != op.rr; ++j)
        idx.emplace_back(op.zero(op.ro + j));
    auto [cur, write_coords]
        = btensor::op_affine_map(mr.map_out, mr.Ro, op.n(), mr.Sr, mr.So, w.tuple(idx), global)->projs<2>();

    DefVec post_elems(op.posts.size());
    for (size_t j = 0; j != op.posts.size(); ++j)
        std::tie(cur, post_elems[j]) = load_at(op.posts[j], k.posts[j], mr.Ro, mr.So, write_coords, cur);

    auto after_post            = w.mut_con(Defs{w.annex<gpu::GlobalM>(), mr.Tp})->set("afterPost");
    auto [post_mem, elem_post] = after_post->vars<2>();
    auto final_mem
        = w.call<mem::store>(Defs{post_mem, mem::op_lea_tuple(k.out, *Shape(write_coords).fold(mr.So)), elem_post});
    after_post->app(true, k.ret, Defs{final_mem, shared, k.const_tok, k.local});
    btensor::app_cps(at, op.post, {cur, acc, w.tuple(post_elems)}, after_post);
}

/// One thread per output point, reducing sequentially over the `rr` reduction dims.
Kernel build_kernel(const Op& op) {
    auto& w        = op.w;
    auto& mr       = op.mr;
    auto global_ty = w.annex<gpu::GlobalM>();

    auto total   = product(op.out_dims);
    auto n_items = std::min<nat_t>(total, 1024);
    auto k       = mk_kernel(op, (total + n_items - 1) / n_items, n_items, nullptr, "mapReduceKernel");
    auto flat    = add_i64(mul_i64(group_i64(k), n_items), item_i64(k));

    auto in_range     = w.call(core::icmp::ul, Defs{flat, w.lit_i64(total)});
    auto early_return = w.mut_con(w.sigma(Defs{}))->set("outOfRange");
    early_return->app(true, k.ret, Defs{k.global, k.shared, k.const_tok, k.local});
    auto body = w.mut_con(w.sigma(Defs{}))->set("inRange");
    k.lam->branch(true, in_range, body, early_return);

    auto write_back          = w.mut_con(Defs{global_ty, mr.To})->set("writeBack");
    auto [wb_mem, acc_final] = write_back->vars<2>();
    auto [wb_mem2, wb_par]   = unflatten_index(w, flat, op.out_dims, wb_mem);
    emit_epilogue(op, k, write_back, wb_mem2, k.shared, acc_final, wb_par);

    const Def* acc  = w.tuple({k.global, mr.init});
    const Def* cont = write_back;
    Lam* cur_lam    = body;
    auto red_exts   = mr.Sr->projs(op.rn());
    auto raw        = affine::counting_nest(cur_lam, cont, acc, Defs(red_exts).subspan(op.ro, op.rr), "forRed");
    auto red_iters  = btensor::to_idxs(w, Defs(red_exts).subspan(op.ro, op.rr), raw);
    auto [red_mem, elem_acc] = acc->projs<2>();

    auto [cur, iters_v] = unflatten_index(w, flat, op.out_dims, red_mem);
    iters_v.insert(iters_v.end(), red_iters.begin(), red_iters.end());
    auto iters = w.tuple(iters_v);

    DefVec elems(op.ins.size());
    for (size_t i = 0; i != op.ins.size(); ++i)
        std::tie(cur, elems[i]) = load_at(op.ins[i], k.ins[i], op.n(), mr.Sr, iters, cur);

    btensor::app_cps(cur_lam, op.comb, {cur, elem_acc, w.tuple(elems)}, cont);
    return k;
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

    DefSet counted;
    for (auto def : old_world().externals().muts())
        count_uses(def, uses_, counted);

    Super::start();
}

const Def* LowerMapReduce::rewrite_imm_App(const App* app) {
    if (Axm::isa<btensor::map_reduce_post>(app)) return lower_map_reduce_post(app);
    return Super::rewrite_imm_App(app);
}

const App* LowerMapReduce::chainable(const Def* def) {
    if (auto i = uses_.find(def); i == uses_.end() || i->second != 1) return nullptr;
    if (auto ex = def->isa<Extract>())
        if (auto app = ex->tuple()->isa<App>(); app && Axm::isa<btensor::map_reduce_post>(app)) {
            // A producer already lowered on its own keeps its host result.
            if (lookup(app) || absorbed_.contains(app)) return nullptr;
            // Nobody may observe the destination of an absorbed producer, so it has to be a fresh buffer.
            if (auto dst = app->arg(4, 1)->isa<Extract>(); dst && Axm::isa<buffer::alloc>(dst->tuple())) return app;
        }
    return nullptr;
}

void LowerMapReduce::plan_inputs(Call& call, bool post, const Def* group, std::deque<Call>& calls) {
    auto& ins = post ? call.posts : call.ins;
    auto n    = ins.size();
    // A group tuple shared between two calls would chain the same producer into both.
    auto shared = n > 1 && uses_[group] != 1;
    for (size_t i = 0; i != n; ++i) {
        auto old      = group->proj(n, i);
        auto producer = shared ? nullptr : chainable(old);
        ins[i]        = {producer ? plan(producer, calls) : nullptr, old};
    }
}

LowerMapReduce::Call* LowerMapReduce::plan(const App* app, std::deque<Call>& calls) {
    auto mr    = btensor::MapRedPost::of(rewrite(app->callee())->as<App>());
    auto ranks = mr.ranks();
    if (!ranks) {
        log().w("{} doesn't have lowering-time known rank counts (nis/nps/Ro/Rn)", app);
        return nullptr;
    }
    auto [nis, nps, ro, rr] = *ranks;

    Vector<nat_t> out_dims(ro);
    for (nat_t d = 0; d != ro; ++d) {
        auto l = Lit::isa<nat_t>(mr.Sr->proj(ro + rr, d));
        if (!l) {
            log().w("{} doesn't have a lowering-time known output (grid) shape", app);
            return nullptr;
        }
        out_dims[d] = *l;
    }
    if (product(out_dims) == 0) {
        log().w("{} has a zero-sized output, skipping GPU lowering", app);
        return nullptr;
    }

    auto comb = mr.comb->isa_mut<Lam>();
    auto post = mr.post->isa_mut<Lam>();
    if (!comb || !post) {
        log().w("{} doesn't have a lowering-time known combiner/epilogue", app);
        return nullptr;
    }

    auto [mem, dst, is, post_is] = app->args<4>();
    Call call{app, mr, ro, rr, out_dims, comb, post, Vector<Call::In>(nis), Vector<Call::In>(nps)};
    plan_inputs(call, false, is, calls);
    plan_inputs(call, true, post_is, calls);
    return &calls.emplace_back(std::move(call));
}

void LowerMapReduce::commit(Call& call, DefVec& hosts) {
    for (auto group : {&call.ins, &call.posts})
        for (auto& in : *group)
            if (in.producer) {
                absorbed_.emplace(in.producer->app);
                commit(*in.producer, hosts);
            } else {
                in.host_idx = hosts.size();
                hosts.emplace_back(in.host);
            }
}

const Def* LowerMapReduce::emit(const Call& call, Session& s) {
    auto& w  = new_world();
    auto& mr = call.mr;

    auto resolve = [&](const Vector<Call::In>& ins, const Def* Rs, const Def* Ss, const Def* Ts, const Def* maps) {
        auto n = ins.size();
        Slots slots(n);
        for (size_t i = 0; i != n; ++i) {
            auto& slot = slots[i] = Slot{Rs->proj(n, i), Ss->proj(n, i), Ts->proj(n, i), maps->proj(n, i), nullptr};
            if (auto producer = ins[i].producer) {
                slot.dptr = emit(*producer, s);
            } else {
                auto copy = w.app(w.app(w.annex<gpu::buf_alloc_copy>(), {slot.r, slot.s, slot.t}),
                                  {s.mem, s.global, s.hosts->proj(s.n_hosts, ins[i].host_idx)});
                std::tie(s.mem, s.global, slot.dptr) = copy->projs<3>();
            }
            s.to_free.emplace_back(slot.dptr);
        }
        return slots;
    };

    auto ins   = resolve(call.ins, mr.Ris, mr.Sis, mr.Tis, mr.maps);
    auto posts = resolve(call.posts, mr.Rps, mr.Sps, mr.Tps, mr.post_maps);

    auto [out_global, out_dptr]
        = w.app(w.app(w.annex<gpu::alloc>(gpu::alloc::block), w.arr(mr.So, mr.Tp)), s.global)->projs<2>();

    Op op{w,
          mr,
          call.ro,
          call.rr,
          call.out_dims,
          std::move(ins),
          std::move(posts),
          rebuild_lam_global_mem(call.comb, mr.To, w.sym("combGlobal")),
          rebuild_lam_global_mem(call.post, mr.Tp, w.sym("postGlobal")),
          out_dptr};
    auto k = build_kernel(op);

    DefVec args;
    for (auto& in : op.ins)
        args.emplace_back(in.dptr);
    for (auto& in : op.posts)
        args.emplace_back(in.dptr);
    args.emplace_back(out_dptr);
    DefVec arg_tys(args.size(), [&](size_t i) { return args[i]->type(); });

    auto launch = w.app(w.annex<gpu::launch>(), Defs{w.lit_nat(args.size()), w.tuple(arg_tys)});
    launch      = w.app(launch, Defs{w.lit_nat(k.n_groups), w.lit_nat(k.n_items), w.annex<gpu::default_stream>(),
                                     w.lit_bool(k.shared_ty != nullptr), k.shared_ty ? k.shared_ty : w.tuple()});
    launch      = w.app(launch, k.lam);
    launch      = w.app(launch, args);

    auto after_launch
        = w.mut_con(Defs{w.call<mem::M>(0), w.annex<gpu::GlobalM>(), w.annex<gpu::ConstM>()})->set("afterLaunch");
    s.cur->set(true, w.app(launch, Defs{w.tuple({s.mem, out_global, s.const_tok}), after_launch}));
    s.cur                                  = after_launch;
    std::tie(s.mem, s.global, s.const_tok) = after_launch->vars<3>();
    return out_dptr;
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    auto& w                        = new_world();
    auto [old_mem, old_dst, _, __] = app->args<4>();

    if (absorbed_.contains(app)) return w.tuple({rewrite(old_mem), rewrite(old_dst)});

    std::deque<Call> calls;
    auto call = plan(app, calls);
    if (!call) return Super::rewrite_imm_App(app);

    // Decide what is absorbed before rewriting any operand: the mem chain leads through the producers.
    DefVec old_hosts;
    commit(*call, old_hosts);
    auto new_mem   = rewrite(old_mem);
    auto new_dst   = rewrite(old_dst);
    auto new_hosts = DefVec(old_hosts.size(), [&](size_t i) { return rewrite(old_hosts[i]); });

    auto [fun, result]
        = btensor::ds_splice(w.tuple({new_mem, new_dst, w.tuple(new_hosts)}), rewrite(app->type()), "mapReduceAffGpu");
    auto [fun_mem, host_buf, fun_hosts] = fun->var(2, 0)->projs<3>();
    auto cont                           = fun->var(2, 1);

    auto [h_mem, h_global, h_const] = w.app(w.annex<gpu::auto_init>(), fun_mem)->projs<3>();
    Session s{fun, h_mem, h_global, h_const, fun_hosts, new_hosts.size(), {}};
    auto out_dptr = emit(*call, s);

    auto& mr = call->mr;
    auto copy
        = w.app(w.app(w.annex<gpu::buf_copy_to_host>(), {mr.Ro, mr.So, mr.Tp}), {s.mem, s.global, out_dptr, host_buf});
    auto [cb_mem, cb_global] = copy->projs<2>();
    for (auto dptr : s.to_free)
        cb_global = w.call(gpu::free::block, Defs{cb_global, dptr});
    cb_global = w.call(gpu::free::block, Defs{cb_global, out_dptr});

    auto final_mem = w.app(w.annex<gpu::auto_deinit>(), Defs{cb_mem, cb_global, s.const_tok});
    s.cur->app(true, cont, Defs{final_mem, host_buf});

    return result;
}

} // namespace mim::plug::gpu::phase
