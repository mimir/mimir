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
    /// For `Concat` and `Pad`, `mr` only holds the shapes and types.
    enum class Kind { MapRed, Concat, Pad } kind = Kind::MapRed;
    nat_t ax                                     = 0; ///< `Concat`: the axis.
    Vector<u64> exts;                                 ///< `Concat`: the inputs' extents along `ax`.
    const Def *lo = nullptr, *fill = nullptr;         ///< `Pad`: the low padding and the literal fill value.
    nat_t readers     = 0;                            ///< Slots reading the result
    nat_t reads       = 0;                            ///< Planning: reads of `result` within the chain.
    const Def* result = nullptr;
    const Def* dptr   = nullptr; ///< Emitting: the result's device buffer, once launched.
};

struct LowerMapReduce::Chain {
    std::deque<Call> calls;
    DefMap<Call*> planned;
    DefSet excluded; ///< Producers also read outside the chain.
};

struct LowerMapReduce::Session {
    Lam* cur; ///< The continuation the next launch is emitted into.
    const Def *mem, *global, *const_tok;
    const Def* hosts;
    size_t n_hosts;
    Vector<nat_t> host_readers;
    DefVec uploaded;
    DefMap<nat_t> pending; ///< Readers left per device buffer.
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
    // A group `(h, h)` normalizes to `‹2; h›` with a single edge to `h`
    if (auto pack = def->isa<Pack>())
        if (auto n = Lit::isa<nat_t>(pack->arity()); n && *n > 1) uses[pack->body()] += *n - 1;
}

/// Whether this phase lowers @p app; all of these take `[mem, dst, …]` and return `[mem, Buf]`.
bool lowerable(const App* app) {
    return Axm::isa<btensor::map_reduce_post>(app) || Axm::isa<btensor::concat>(app) || Axm::isa<btensor::pad>(app);
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

Vector<nat_t> row_major_strides(const Vector<nat_t>& dims) {
    Vector<nat_t> strides(dims.size());
    nat_t acc = 1;
    for (auto i = dims.size(); i-- != 0;) {
        strides[i] = acc;
        acc *= dims[i];
    }
    return strides;
}

const Def* add_i64(const Def* a, const Def* b) {
    return a->world().call(core::wrap::add, core::Mode::none, Defs{a, b});
}

const Def* mul_i64(const Def* a, nat_t b) {
    auto& w = a->world();
    return w.call(core::wrap::mul, core::Mode::none, Defs{a, w.lit_i64(b)});
}

/// `mem.lea` with an `I64` index, converted to the pointee's index type.
const Def* lea_i64(const Def* ptr, const Def* i) {
    auto& w = ptr->world();
    return mem::op_lea(ptr, w.call(core::conv::u, Axm::as<mem::Ptr>(ptr->type())->arg(2, 0)->arity(), i));
}

/// `{mem', x / c, x mod c}` on the `I64` @p x.
std::tuple<const Def*, const Def*, const Def*> divmod(const Def* mem, const Def* x, nat_t c) {
    auto& w      = x->world();
    auto [m1, q] = w.call(core::div::udiv, Defs{mem, w.tuple({x, w.lit_i64(c)})})->projs<2>();
    auto [m2, r] = w.call(core::div::urem, Defs{m1, w.tuple({x, w.lit_i64(c)})})->projs<2>();
    return {m2, q, r};
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

/// One thread per output cell, which stores `compute(k, iters, mem)`'s element there; `iters` are the cell's `I64`
/// coordinates and `compute` threads its loads through `mem`.
Kernel build_kernel_pointwise(const Op& op, std::string name, auto compute) {
    auto& w  = op.w;
    auto& mr = op.mr;

    auto total   = product(op.out_dims);
    auto n_items = std::min<nat_t>(total, 1024);
    auto k       = mk_kernel(op, (total + n_items - 1) / n_items, n_items, nullptr, std::move(name));
    auto flat    = add_i64(mul_i64(group_i64(k), n_items), item_i64(k));

    auto in_range     = w.call(core::icmp::ul, Defs{flat, w.lit_i64(total)});
    auto early_return = w.mut_con(w.sigma(Defs{}))->set("outOfRange");
    early_return->app(true, k.ret, Defs{k.global, k.shared, k.const_tok, k.local});
    auto body = w.mut_con(w.sigma(Defs{}))->set("inRange");
    k.lam->branch(true, in_range, body, early_return);

    auto [cur, coords] = unflatten_index(w, flat, op.out_dims, k.global);
    auto iters = DefVec(coords.size(), [&](size_t d) { return w.call(core::conv::u, w.lit_nat_0(), coords[d]); });

    auto elem      = compute(k, iters, cur);
    auto final_mem = w.call<mem::store>(Defs{cur, mem::op_lea_tuple(k.out, btensor::folded_idx(mr.So, iters)), elem});
    body->app(true, k.ret, Defs{final_mem, k.shared, k.const_tok, k.local});
    return k;
}

/// `btensor.concat` along @p ax: every cell copies the cell of the input owning it.
Kernel build_kernel_concat(const Op& op, nat_t ax, const Vector<u64>& exts) {
    auto& w = op.w;
    return build_kernel_pointwise(op, "concatKernel", [&](const Kernel& k, const DefVec& iters, const Def*& mem) {
        auto read = [&](u64 i, const Def* ax_idx) {
            auto in_iters  = iters;
            in_iters[ax]   = ax_idx;
            auto ptr       = mem::op_lea_tuple(k.ins[i], btensor::folded_idx(op.ins[i].s, in_iters));
            auto [m, elem] = w.call<mem::load>(Defs{mem, ptr})->projs<2>();
            mem            = m;
            return elem;
        };
        return btensor::concat_select(iters[ax], exts, read);
    });
}

/// Constant-mode `btensor.pad`: every cell reads the input at its coordinates shifted by @p lo, or takes @p fill
/// outside the input.
Kernel build_kernel_pad(const Op& op, const Def* lo, const Def* fill) {
    auto& w = op.w;
    return build_kernel_pointwise(op, "padKernel", [&](const Kernel& k, const DefVec& iters, const Def*& mem) {
        auto s_in            = op.ins[0].s;
        auto [coords, valid] = btensor::pad_coords(iters, lo, s_in, false);
        auto ptr             = mem::op_lea_tuple(k.ins[0], *Shape(w, coords).fold(s_in));
        auto [m, elem]       = w.call<mem::load>(Defs{mem, ptr})->projs<2>();
        mem                  = m;
        return w.select(valid, elem, fill);
    });
}

/// Whether the access map @p map reads loop position @p pos: it is evaluated on two marker vectors differing only
/// there.
bool depends_on_axis(const Def* map, nat_t rn, nat_t pos) {
    auto& w     = map->world();
    auto base   = DefVec(rn, [&](size_t i) { return w.call<affine::lit>(w.lit_nat(i + 1)); });
    auto varied = base;
    varied[pos] = w.call<affine::lit>(w.lit_nat(rn + 1));
    return w.app(map, w.tuple(base)) != w.app(map, w.tuple(varied));
}

/// How two inputs that both read every reduction position split the parallel positions between them.
struct Split {
    Vector<nat_t> pos0, pos1; ///< Ascending; together they partition `[0, ro)`.
};

std::optional<Split> split_parallel(const Op& op) {
    if (op.ins.size() != 2 || op.rr < 1 || op.ro < 2) return {};
    auto rn = op.rn();
    for (auto& in : op.ins)
        for (nat_t p = op.ro; p != rn; ++p)
            if (!depends_on_axis(in.map, rn, p)) return {};

    Split split;
    for (nat_t p = 0; p != op.ro; ++p) {
        auto d0 = depends_on_axis(op.ins[0].map, rn, p);
        auto d1 = depends_on_axis(op.ins[1].map, rn, p);
        if (d0 == d1) return {};
        (d0 ? split.pos0 : split.pos1).emplace_back(p);
    }
    if (split.pos0.empty() || split.pos1.empty()) return {};
    return split;
}

/// Thread-block edge length of `build_kernel_gemm`'s shared-memory tiles.
constexpr nat_t Gemm_Tile = 16;

/// A shared-memory tiled contraction: input 0 owns the rows `split.pos0`, input 1 the columns `split.pos1`.
/// Each `Gemm_Tile`² thread block covers one output tile and streams the reduction through shared memory a tile at a
/// time, so the block reads every row and column from global memory once.
/// All three flattened extents must be multiples of `Gemm_Tile`.
Kernel build_kernel_gemm(const Op& op, const Split& split, const Vector<nat_t>& k_ext) {
    auto& w        = op.w;
    auto& mr       = op.mr;
    auto global_ty = w.annex<gpu::GlobalM>();
    auto shared_m  = w.annex<gpu::SharedM>();
    auto tile      = Gemm_Tile;
    auto rn        = op.rn();

    Vector<nat_t> m_ext, n_ext;
    for (auto p : split.pos0)
        m_ext.emplace_back(op.out_dims[p]);
    for (auto p : split.pos1)
        n_ext.emplace_back(op.out_dims[p]);
    auto n_blocks0 = product(m_ext) / tile;
    auto n_blocks1 = product(n_ext) / tile;

    // The row stride `tile + 1` staggers same-column accesses across shared-memory banks.
    auto tile_ty   = [&](const Def* t) { return w.arr(w.lit_nat(tile), w.arr(w.lit_nat(tile + 1), t)); };
    auto shared_ty = w.sigma({tile_ty(op.ins[0].t), tile_ty(op.ins[1].t)});
    auto k         = mk_kernel(op, n_blocks0 * n_blocks1, tile * tile, shared_ty, "gemmTiledKernel");
    auto shared0   = lea_i64(k.shared_ptr, w.lit_i64(0));
    auto shared1   = lea_i64(k.shared_ptr, w.lit_i64(1));

    auto [g0, block0, block1] = divmod(k.global, group_i64(k), n_blocks1);
    auto [g1, ty, tx]         = divmod(g0, item_i64(k), tile);
    auto [g2, m_coords]       = unflatten_index(w, add_i64(mul_i64(block0, tile), ty), m_ext, g1);
    auto [entry, n_coords]    = unflatten_index(w, add_i64(mul_i64(block1, tile), tx), n_ext, g2);

    auto ty_idx = w.call(core::conv::u, w.lit_nat(tile), ty);
    auto tx_idx = w.call(core::conv::u, w.lit_nat(tile), tx);
    auto ty_pad = w.call(core::conv::u, w.lit_nat(tile + 1), ty);
    auto tx_pad = w.call(core::conv::u, w.lit_nat(tile + 1), tx);

    // A position the input does not read may hold any in-bounds value.
    auto iters = [&](const Vector<nat_t>& pos, const DefVec& coords, const DefVec& k_vals) {
        auto iv = DefVec(rn, [&](size_t p) { return op.zero(p); });
        for (size_t j = 0; j != pos.size(); ++j)
            iv[pos[j]] = coords[j];
        for (nat_t j = 0; j != op.rr; ++j)
            iv[op.ro + j] = k_vals[j];
        return w.tuple(iv);
    };

    // The loops below carry only the shared memory: `affine.lower_for` threads a single memory through a loop.
    auto write_back          = w.mut_con(Defs{shared_m, mr.To})->set("gemmWriteBack");
    auto [wb_shared, wb_acc] = write_back->vars<2>();
    auto par                 = DefVec(op.ro);
    for (size_t j = 0; j != split.pos0.size(); ++j)
        par[split.pos0[j]] = m_coords[j];
    for (size_t j = 0; j != split.pos1.size(); ++j)
        par[split.pos1[j]] = n_coords[j];
    emit_epilogue(op, k, write_back, entry, wb_shared, wb_acc, par);

    auto [outer, outer_for] = affine::counting_for(w.lit_i64(product(k_ext) / tile), w.tuple({k.shared, mr.init}),
                                                   write_back, w.sym("gemmKTile"));
    k.lam->set(true, outer_for);
    auto [kt, outer_acc, outer_yield] = outer->vars<3>();
    auto [s0, acc0]                   = outer_acc->projs<2>();
    auto k_base                       = mul_i64(kt, tile);

    auto [g3, k0]   = unflatten_index(w, add_i64(k_base, tx), k_ext, entry);
    auto [g4, val0] = load_at(op.ins[0], k.ins[0], op.n(), mr.Sr, iters(split.pos0, m_coords, k0), g3);
    auto [g5, k1]   = unflatten_index(w, add_i64(k_base, ty), k_ext, g4);
    auto [g6, val1] = load_at(op.ins[1], k.ins[1], op.n(), mr.Sr, iters(split.pos1, n_coords, k1), g5);

    auto s1       = w.call<mem::store>(Defs{s0, mem::op_lea_tuple(shared0, w.tuple({ty_idx, tx_pad})), val0});
    auto s2       = w.call<mem::store>(Defs{s1, mem::op_lea_tuple(shared1, w.tuple({tx_idx, ty_pad})), val1});
    auto [g7, s3] = w.app(w.annex<gpu::sync_work_items>(), Defs{g6, s2})->projs<2>();

    auto after_inner = w.mut_con(Defs{global_ty, mr.To})->set("gemmAfterInner");
    auto [inner, inner_for]
        = affine::counting_for(w.lit_i64(tile), w.tuple({g7, acc0}), after_inner, w.sym("gemmInnerK"));
    outer->set(true, inner_for);
    auto [kk, inner_acc, inner_yield] = inner->vars<3>();
    auto [gi, acci]                   = inner_acc->projs<2>();
    auto kk_idx                       = w.call(core::conv::u, w.lit_nat(tile + 1), kk);

    auto [r0, v0] = w.call<mem::load>(Defs{s3, mem::op_lea_tuple(shared0, w.tuple({ty_idx, kk_idx}))})->projs<2>();
    auto [r1, v1] = w.call<mem::load>(Defs{r0, mem::op_lea_tuple(shared1, w.tuple({tx_idx, kk_idx}))})->projs<2>();
    btensor::app_cps(inner, op.comb, {gi, acci, w.tuple({v0, v1})}, inner_yield);

    // The block must finish reading the tile before the next round overwrites it.
    auto [ai_mem, ai_acc] = after_inner->vars<2>();
    auto s4               = w.app(w.annex<gpu::sync_work_items>(), Defs{ai_mem, s3})->proj(2, 1);
    after_inner->app(true, outer_yield, Defs{s4, ai_acc});

    return k;
}

/// A CUDA block holds at most 1024 threads.
constexpr nat_t Conv_Max_Ow = 1024;
/// Only `ow` threads load the filter per round, so a large filter over a narrow row would load for too long.
constexpr nat_t Conv_Max_Load_Iters = 8;

/// A convolution-shaped reduction: the weight input reads the parallel positions `weight_pos`, the data input the rest.
/// A thread block is one output row; it loads its whole filter into shared memory once and every thread reads the
/// weights from there, while the data input is read along the row with unit stride.
Kernel build_kernel_conv(const Op& op,
                         nat_t weight_idx,
                         const Vector<nat_t>& weight_pos,
                         const Vector<nat_t>& data_pos,
                         const Vector<nat_t>& filter_ext) {
    auto& w        = op.w;
    auto& mr       = op.mr;
    auto global_ty = w.annex<gpu::GlobalM>();
    auto shared_m  = w.annex<gpu::SharedM>();
    auto data_idx  = 1 - weight_idx;
    auto& weight   = op.ins[weight_idx];

    // A schedule only splits lower positions, so the last data position is still the fastest-varying one.
    auto ow_pos = data_pos.back();
    auto ow     = op.out_dims[ow_pos];

    auto outer_pos = weight_pos;
    for (auto p : data_pos)
        if (p != ow_pos) outer_pos.emplace_back(p);
    std::ranges::sort(outer_pos);
    Vector<nat_t> outer_ext;
    for (auto p : outer_pos)
        outer_ext.emplace_back(op.out_dims[p]);

    auto filter_total   = product(filter_ext);
    auto filter_strides = row_major_strides(filter_ext);
    // Padding the cache to whole rounds lets every round load unconditionally; the surplus slots are never read.
    auto rounds = (filter_total + ow - 1) / ow;

    auto shared_ty = w.arr(w.lit_nat(rounds * ow), weight.t);
    auto k         = mk_kernel(op, product(outer_ext), ow, shared_ty, "convTiledKernel");
    auto item      = item_i64(k);

    auto [entry, outer_coords] = unflatten_index(w, group_i64(k), outer_ext, k.global);
    auto par                   = DefVec(op.ro);
    for (size_t j = 0; j != outer_pos.size(); ++j)
        par[outer_pos[j]] = outer_coords[j];
    par[ow_pos] = w.call(core::conv::u, op.extent(ow_pos), item);

    auto iters = [&](const DefVec& red) {
        auto iv = par;
        iv.insert(iv.end(), red.begin(), red.end());
        return w.tuple(iv);
    };

    // The load loop carries only the shared memory: `affine.lower_for` threads a single memory through a loop.
    auto after_load       = w.mut_con(shared_m)->set("convAfterLoad");
    auto [load, load_for] = affine::counting_for(w.lit_i64(rounds), k.shared, after_load, w.sym("convWeightLoad"));
    k.lam->set(true, load_for);
    auto [round, load_shared, load_yield] = load->vars<3>();

    auto load_flat         = add_i64(mul_i64(round, ow), item);
    auto [lm, load_coords] = unflatten_index(w, load_flat, filter_ext, entry);
    auto load_val          = load_at(weight, k.ins[weight_idx], op.n(), mr.Sr, iters(load_coords), lm).second;
    load->app(true, load_yield, w.call<mem::store>(Defs{load_shared, lea_i64(k.shared_ptr, load_flat), load_val}));

    auto [bar_mem, bar_shared] = w.app(w.annex<gpu::sync_work_items>(), Defs{entry, after_load->var()})->projs<2>();

    auto write_back          = w.mut_con(Defs{global_ty, mr.To})->set("convWriteBack");
    auto [wb_mem, acc_final] = write_back->vars<2>();
    emit_epilogue(op, k, write_back, wb_mem, bar_shared, acc_final, par);

    const Def* acc  = w.tuple({bar_mem, mr.init});
    const Def* cont = write_back;
    Lam* cur_lam    = after_load;
    auto red_exts   = mr.Sr->projs(op.rn());
    auto raw        = affine::counting_nest(cur_lam, cont, acc, Defs(red_exts).subspan(op.ro, op.rr), "convRed");
    auto red        = btensor::to_idxs(w, Defs(red_exts).subspan(op.ro, op.rr), raw);
    auto [red_mem, elem_acc] = acc->projs<2>();

    auto [data_mem, data_val] = load_at(op.ins[data_idx], k.ins[data_idx], op.n(), mr.Sr, iters(red), red_mem);

    const Def* weight_flat = w.lit_i64(0);
    for (nat_t j = 0; j != op.rr; ++j)
        weight_flat = add_i64(weight_flat, mul_i64(raw[j], filter_strides[j]));
    auto weight_val = w.call<mem::load>(Defs{bar_shared, lea_i64(k.shared_ptr, weight_flat)})->proj(2, 1);

    DefVec elems(2);
    elems[data_idx]   = data_val;
    elems[weight_idx] = weight_val;
    btensor::app_cps(cur_lam, op.comb, {data_mem, elem_acc, w.tuple(elems)}, cont);

    return k;
}

/// Picks the most specific kernel the op's structure allows.
Kernel select_kernel(const Op& op) {
    Vector<nat_t> red_ext;
    for (nat_t j = 0; j != op.rr; ++j)
        if (auto l = Lit::isa<nat_t>(op.extent(op.ro + j)))
            red_ext.emplace_back(*l);
        else
            return build_kernel(op);

    if (auto split = split_parallel(op)) {
        auto extent_of = [&](const Vector<nat_t>& pos) {
            nat_t total = 1;
            for (auto p : pos)
                total *= op.out_dims[p];
            return total;
        };
        auto tiles = [](nat_t extent) { return extent != 0 && extent % Gemm_Tile == 0; };

        // Flattening a multi-dim reduction into the tiled K loop is slower than caching the whole filter.
        if (op.rr == 1 && tiles(extent_of(split->pos0)) && tiles(extent_of(split->pos1)) && tiles(red_ext[0]))
            return build_kernel_gemm(op, *split, red_ext);

        // A schedule's neutral splits leave size-1 positions behind; the weight reads one real position.
        auto single = [&](const Vector<nat_t>& pos) {
            return std::ranges::count_if(pos, [&](nat_t p) { return op.out_dims[p] != 1; }) == 1;
        };
        if (op.rr >= 2 && single(split->pos0) != single(split->pos1)) {
            nat_t weight_idx = single(split->pos0) ? 0 : 1;
            auto& weight_pos = weight_idx == 0 ? split->pos0 : split->pos1;
            auto& data_pos   = weight_idx == 0 ? split->pos1 : split->pos0;
            auto ow          = op.out_dims[data_pos.back()];
            auto total       = product(red_ext);
            if (total > 1 && ow <= Conv_Max_Ow && (total + ow - 1) / ow <= Conv_Max_Load_Iters)
                return build_kernel_conv(op, weight_idx, weight_pos, data_pos, red_ext);
        }
    }

    return build_kernel(op);
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
    if (lowerable(app)) return lower_map_reduce_post(app);
    return Super::rewrite_imm_App(app);
}

const App* LowerMapReduce::chainable(const Def* def, const Chain& chain) {
    if (!uses_.contains(def)) return nullptr;
    if (auto ex = def->isa<Extract>())
        if (auto app = ex->tuple()->isa<App>(); app && lowerable(app)) {
            // Lowered on its own or read outside the chain: keeps its host result.
            if (lookup(app) || absorbed_.contains(app) || chain.excluded.contains(app)) return nullptr;
            // Nobody may observe the destination of an absorbed producer, so it has to be a fresh buffer.
            auto dst = app->arg(app->num_args(), 1)->isa<Extract>();
            if (dst && Axm::isa<buffer::alloc>(dst->tuple())) return app;
        }
    return nullptr;
}

void LowerMapReduce::plan_inputs(Call& call, bool post, const Def* group, Chain& chain) {
    auto& ins = post ? call.posts : call.ins;
    auto n    = ins.size();
    // A group tuple shared between two calls would chain the same producer into both.
    auto shared = n > 1 && uses_[group] != 1;
    for (size_t i = 0; i != n; ++i) {
        auto old      = group->proj(n, i);
        auto producer = shared ? nullptr : chainable(old, chain);
        ins[i]        = {producer ? plan(producer, chain) : nullptr, old};
    }
}

LowerMapReduce::Call* LowerMapReduce::plan_concat(const App* app, Chain& chain) {
    auto& w                      = new_world();
    auto [TnisR, ax, Sis, s_out] = rewrite(app->callee())->as<App>()->uncurry_args<4>();
    auto [T, nis, r]             = TnisR->projs<3>();

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) {
        log().w("{} doesn't have a lowering-time known input count, rank and axis", app);
        return nullptr;
    }
    auto exts = btensor::concat_extents(Sis, *nis_l, *r_l, *ax_l);

    Vector<nat_t> out_dims(*r_l);
    for (nat_t d = 0; d != *r_l; ++d)
        if (auto l = Lit::isa<nat_t>(s_out->proj(*r_l, d)))
            out_dims[d] = *l;
        else
            exts.reset();
    if (!exts || product(out_dims) == 0) {
        log().w("{} doesn't have a lowering-time known, non-empty shape", app);
        return nullptr;
    }

    btensor::MapRedPost mr{};
    mr.Ro = mr.Rn = r;
    mr.So = mr.Sr = s_out;
    mr.To = mr.Tp = T;
    mr.Sis        = Sis;
    mr.Ris        = w.pack(*nis_l, r);
    mr.Tis        = w.pack(*nis_l, T);

    Call call{app, mr, *r_l, 0, out_dims, nullptr, nullptr, Vector<Call::In>(*nis_l), Vector<Call::In>(0)};
    call.kind = Call::Kind::Concat;
    call.ax   = *ax_l;
    call.exts = *exts;
    plan_inputs(call, false, app->arg(3, 2), chain);
    return &chain.calls.emplace_back(std::move(call));
}

LowerMapReduce::Call* LowerMapReduce::plan_pad(const App* app, Chain& chain) {
    auto [Tr, s_in, params, s_out] = rewrite(app->callee())->as<App>()->uncurry_args<4>();
    auto [T, r]                    = Tr->projs<2>();
    auto [mode, lo, hi]            = params->projs<3>();

    auto r_l    = Lit::isa<u64>(r);
    auto mode_l = Lit::isa<u64>(mode);
    if (!r_l || !mode_l) {
        log().w("{} doesn't have a lowering-time known rank and mode", app);
        return nullptr;
    }

    Vector<nat_t> out_dims(*r_l);
    for (nat_t d = 0; d != *r_l; ++d)
        if (auto l = Lit::isa<nat_t>(s_out->proj(*r_l, d)))
            out_dims[d] = *l;
        else
            r_l.reset();
    if (!r_l || product(out_dims) == 0) {
        log().w("{} doesn't have a lowering-time known, non-empty shape", app);
        return nullptr;
    }

    // Only constant mode, and the kernel cannot read a host scalar: the fill value has to be a literal.
    auto fill = rewrite(app->arg(4, 3));
    if (*mode_l != 0 || !fill->isa<Lit>()) return nullptr;

    btensor::MapRedPost mr{};
    mr.Ro = mr.Rn = mr.Ris = r;
    mr.So = mr.Sr = s_out;
    mr.To = mr.Tp = mr.Tis = T;
    mr.Sis                 = s_in;

    Call call{app, mr, *r_l, 0, out_dims, nullptr, nullptr, Vector<Call::In>(1), Vector<Call::In>(0)};
    call.kind = Call::Kind::Pad;
    call.lo   = lo;
    call.fill = fill;
    plan_inputs(call, false, app->arg(4, 2), chain);
    return &chain.calls.emplace_back(std::move(call));
}

LowerMapReduce::Call* LowerMapReduce::plan(const App* app, Chain& chain) {
    if (auto i = chain.planned.find(app); i != chain.planned.end()) return i->second;
    auto call                 = Axm::isa<btensor::concat>(app) ? plan_concat(app, chain)
                              : Axm::isa<btensor::pad>(app)    ? plan_pad(app, chain)
                                                               : plan_map_reduce(app, chain);
    return chain.planned[app] = call;
}

LowerMapReduce::Call* LowerMapReduce::plan_map_reduce(const App* app, Chain& chain) {
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
    plan_inputs(call, false, is, chain);
    plan_inputs(call, true, post_is, chain);
    return &chain.calls.emplace_back(std::move(call));
}

void LowerMapReduce::commit(Call& call, DefVec& hosts, DefMap<size_t>& host_index, Vector<nat_t>& host_readers) {
    for (auto group : {&call.ins, &call.posts})
        for (auto& in : *group)
            if (in.producer) {
                if (in.producer->readers++ == 0) {
                    absorbed_.emplace(in.producer->app);
                    commit(*in.producer, hosts, host_index, host_readers);
                }
            } else {
                auto [i, fresh] = host_index.emplace(in.host, hosts.size());
                if (fresh) {
                    hosts.emplace_back(in.host);
                    host_readers.emplace_back(0);
                }
                in.host_idx = i->second;
                ++host_readers[in.host_idx];
            }
}

const Def* LowerMapReduce::emit(Call& call, Session& s) {
    if (call.dptr) return call.dptr;
    auto& w  = new_world();
    auto& mr = call.mr;

    auto resolve = [&](const Vector<Call::In>& ins, const Def* Rs, const Def* Ss, const Def* Ts, const Def* maps) {
        auto n = ins.size();
        Slots slots(n);
        for (size_t i = 0; i != n; ++i) {
            auto map   = maps ? maps->proj(n, i) : nullptr;
            auto& slot = slots[i] = Slot{Rs->proj(n, i), Ss->proj(n, i), Ts->proj(n, i), map, nullptr};
            if (auto producer = ins[i].producer) {
                slot.dptr = emit(*producer, s);
            } else if (auto& dptr = s.uploaded[ins[i].host_idx]) {
                slot.dptr = dptr;
            } else {
                auto copy = w.app(w.app(w.annex<gpu::buf_alloc_copy>(), {slot.r, slot.s, slot.t}),
                                  {s.mem, s.global, s.hosts->proj(s.n_hosts, ins[i].host_idx)});
                std::tie(s.mem, s.global, slot.dptr) = copy->projs<3>();
                s.pending[slot.dptr]                 = s.host_readers[ins[i].host_idx];
                dptr                                 = slot.dptr;
            }
        }
        return slots;
    };

    auto ins     = resolve(call.ins, mr.Ris, mr.Sis, mr.Tis, mr.maps);
    auto map_red = call.kind == Call::Kind::MapRed;
    auto posts   = map_red ? resolve(call.posts, mr.Rps, mr.Sps, mr.Tps, mr.post_maps) : Slots();

    auto [out_global, out_dptr]
        = w.app(w.app(w.annex<gpu::alloc>(gpu::alloc::block), w.arr(mr.So, mr.Tp)), s.global)->projs<2>();
    s.pending[out_dptr] = call.readers;

    Op op{w,
          mr,
          call.ro,
          call.rr,
          call.out_dims,
          std::move(ins),
          std::move(posts),
          map_red ? rebuild_lam_global_mem(call.comb, mr.To, w.sym("combGlobal")) : nullptr,
          map_red ? rebuild_lam_global_mem(call.post, mr.Tp, w.sym("postGlobal")) : nullptr,
          out_dptr};
    auto k = call.kind == Call::Kind::Concat ? build_kernel_concat(op, call.ax, call.exts)
           : call.kind == Call::Kind::Pad    ? build_kernel_pad(op, call.lo, call.fill)
                                             : select_kernel(op);

    DefVec args;
    for (auto& in : op.ins)
        args.emplace_back(in.dptr);
    for (auto& in : op.posts)
        args.emplace_back(in.dptr);
    args.emplace_back(out_dptr);
    DefVec arg_tys(args.size(), [&](size_t i) { return args[i]->type(); });

    auto launch = w.app(w.annex<gpu::launch>(),
                        Defs{w.lit_nat(k.shared_ty ? 1 : 0), k.shared_ty ? k.shared_ty : w.tuple(), w.sigma(arg_tys)});
    launch      = w.app(launch, Defs{w.lit_nat(k.n_groups), w.lit_nat(k.n_items), w.annex<gpu::default_stream>()});
    launch      = w.app(launch, k.lam);
    launch      = w.app(launch, args);

    auto after_launch
        = w.mut_con(Defs{w.call<mem::M>(0), w.annex<gpu::GlobalM>(), w.annex<gpu::ConstM>()})->set("afterLaunch");
    s.cur->set(true, w.app(launch, Defs{w.tuple({s.mem, out_global, s.const_tok}), after_launch}));
    s.cur                                  = after_launch;
    std::tie(s.mem, s.global, s.const_tok) = after_launch->vars<3>();
    // Free each input after its last reader.
    for (size_t i = 0, e = args.size() - 1; i != e; ++i)
        if (--s.pending[args[i]] == 0) s.global = w.call(gpu::free::block, Defs{s.global, args[i]});
    return call.dptr = out_dptr;
}

const Def* LowerMapReduce::lower_map_reduce_post(const App* app) {
    auto& w      = new_world();
    auto n_args  = app->num_args();
    auto old_mem = app->arg(n_args, 0);
    auto old_dst = app->arg(n_args, 1);

    if (absorbed_.contains(app)) return w.tuple({rewrite(old_mem), rewrite(old_dst)});

    // Replan until no absorbed producer is also read outside the chain.
    Chain chain;
    Call* call = nullptr;
    for (;;) {
        chain.calls.clear();
        chain.planned.clear();
        if (!(call = plan(app, chain))) return Super::rewrite_imm_App(app);
        auto count = [&](auto& self, Call& c) -> void {
            for (auto group : {&c.ins, &c.posts})
                for (auto& in : *group)
                    if (in.producer && (in.producer->result = in.host, in.producer->reads++ == 0))
                        self(self, *in.producer);
        };
        count(count, *call);
        auto stable = true;
        for (auto& c : chain.calls)
            if (&c != call && c.reads != uses_[c.result]) chain.excluded.emplace(c.app), stable = false;
        if (stable) break;
    }

    // Decide what is absorbed before rewriting any operand: the mem chain leads through the producers.
    // Not members: rewriting the operands lowers earlier chains recursively.
    DefVec old_hosts;
    DefMap<size_t> host_index;
    Vector<nat_t> host_readers;
    commit(*call, old_hosts, host_index, host_readers);
    auto new_mem   = rewrite(old_mem);
    auto new_dst   = rewrite(old_dst);
    auto new_hosts = DefVec(old_hosts.size(), [&](size_t i) { return rewrite(old_hosts[i]); });

    auto [fun, result]
        = btensor::ds_splice(w.tuple({new_mem, new_dst, w.tuple(new_hosts)}), rewrite(app->type()), "mapReduceAffGpu");
    auto [fun_mem, host_buf, fun_hosts] = fun->var(2, 0)->projs<3>();
    auto cont                           = fun->var(2, 1);

    auto [h_mem, h_global, h_const] = w.app(w.annex<gpu::auto_init>(), fun_mem)->projs<3>();
    Session s{fun,
              h_mem,
              h_global,
              h_const,
              fun_hosts,
              new_hosts.size(),
              std::move(host_readers),
              DefVec(new_hosts.size(), nullptr),
              {}};
    auto out_dptr = emit(*call, s);

    auto& mr = call->mr;
    auto copy
        = w.app(w.app(w.annex<gpu::buf_copy_to_host>(), {mr.Ro, mr.So, mr.Tp}), {s.mem, s.global, out_dptr, host_buf});
    auto [cb_mem, cb_global] = copy->projs<2>();
    cb_global                = w.call(gpu::free::block, Defs{cb_global, out_dptr});

    auto final_mem = w.app(w.annex<gpu::auto_deinit>(), Defs{cb_mem, cb_global, s.const_tok});
    s.cur->app(true, cont, Defs{final_mem, host_buf});

    return result;
}

} // namespace mim::plug::gpu::phase
