#include "mim/plug/tensor/phase/lower_to_mem.h"

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"

#include "mim/plug/buffer/buffer.h"
#include "mim/plug/core/core.h"
#include "mim/plug/matrix/matrix.h"
#include "mim/plug/mem/mem.h"
#include "mim/plug/tensor/tensor.h"

namespace mim::plug::tensor::phase {

namespace {

/// Does `t` (recursively through immutable sigmas) contain a Pi?
bool contains_pi(const Def* t) {
    if (t->isa<Pi>()) return true;
    if (auto sig = t->isa_imm<Sigma>())
        for (auto op : sig->ops())
            if (contains_pi(op)) return true;
    return false;
}

/// Peels nested `Pack`s off `d`; returns the innermost body iff `d` is a constant splat — every axis a
/// `Pack` (so `d` is uniform, not e.g. a `Tuple` of distinct rows), bottoming out in a closed scalar
/// (index-independent, non-array). So `‹784, 1024; 1e-3›` yields the scalar `1e-3`, while an index-dependent
/// pack `‹i; f i›` or a genuine literal `((1, 2), (3, 4))` yields `nullptr`.
const Def* splat_scalar(const Def* d) {
    if (!d->isa<Pack>()) return nullptr;
    while (auto pack = d->isa<Pack>())
        d = pack->body();
    return (d->is_closed() && !d->type()->isa<Arr>()) ? d : nullptr;
}

/// Is `app` one of the tensor ops this phase bufferizes?
bool is_tensor_op(const App* app) {
    return Axm::isa<tensor::get>(app) || Axm::isa<tensor::set>(app) || Axm::isa<tensor::broadcast>(app)
        || Axm::isa<tensor::map_reduce>(app) || Axm::isa<tensor::pad>(app) || Axm::isa<tensor::concat>(app);
}

} // namespace

void LowerToMem::collect_tensor_types() {
    // The default pipeline lowers tensors exclusively through buffers — there is no value-semantics
    // fallback. A program shape the conversion cannot handle is a hard error, not silent residue.
    auto gate = [](const char* why, const Def* culprit) { fe::throwf("cannot bufferize: {} ({})", why, culprit); };
    // Fully folded shapes (`«1; T»` ≡ `T`) denote plain scalars — recording them would poison every
    // function whose signature mentions the element type, so only genuine array types count as tensors.
    auto add_tensor_ty = [this](const Def* t) {
        if (t->isa<Arr>()) tensor_ty_.emplace(t);
    };

    unique_queue<DefSet> wl;
    auto push = [&wl](const Def* d) {
        if (d) wl.push(d);
    };

    for (auto mut : old_world().externals().muts())
        push(mut);

    while (!wl.empty()) {
        auto def = wl.pop();

        if (auto app = def->isa<App>()) {
            if (auto [axm, curry, trip] = Axm::get(app); axm && curry == 0 && axm->plugin() == tensor::Plugin_Id)
                ops_seen_ = true;
            if (Axm::isa<tensor::get>(app) || Axm::isa<tensor::set>(app)) {
                // get/set: the first explicit argument is the tensor `arr`.
                auto [T, r, s] = app->callee()->as<App>()->args<3>();
                if (T->isa<Arr>()) gate("tensor with array element type", T);
                add_tensor_ty(app->arg()->proj(0)->type());
            } else if (Axm::isa<tensor::map_reduce>(app)) {
                // result and each of the `nis` inputs are tensors.
                add_tensor_ty(app->type());
                auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs]
                    = app->callee()->as<App>()->uncurry_args<7>();
                if (meta->proj(3, 0)->isa<Arr>()) gate("tensor with array element type", meta->proj(3, 0));
                if (auto nis_l = Lit::isa<u64>(nis)) {
                    auto Tis = TisRisSis->proj(3, 0);
                    for (u64 i = 0; i < *nis_l; ++i) {
                        if (Tis->proj(*nis_l, i)->isa<Arr>()) gate("tensor with array element type", Tis);
                        add_tensor_ty(app->arg()->proj(*nis_l, i)->type());
                    }
                }
            } else if (Axm::isa<tensor::broadcast>(app)) {
                // result «s_out; T» and input «s_in; T» (the 3rd argument) are tensors.
                auto [T, r] = app->callee()->as<App>()->args<2>();
                if (T->isa<Arr>()) gate("tensor with array element type", T);
                add_tensor_ty(app->type());
                add_tensor_ty(app->arg()->proj(2)->type());
            } else if (Axm::isa<tensor::pad>(app)) {
                // callee: pad {T, r} [s_in] [mode, lo, hi]; result «s_out; T» and `input` are tensors.
                auto [Tr, s_in, params] = app->callee()->as<App>()->uncurry_args<3>();
                auto [T, r]             = Tr->projs<2>();
                if (T->isa<Arr>()) gate("tensor with array element type", T);
                if (!Lit::isa<u64>(r) || !Lit::isa<u64>(params->proj(3, 0)))
                    gate("non-literal rank/mode of %tensor.pad", app);
                add_tensor_ty(app->type());
                add_tensor_ty(app->arg()->proj(0)->type());
            } else if (Axm::isa<tensor::concat>(app)) {
                // callee: concat {T, nis, r} [ax] {Sis}; result «s_out; T» and each input are tensors.
                auto [TnisR, ax, Sis] = app->callee()->as<App>()->uncurry_args<3>();
                auto [T, nis, r]      = TnisR->projs<3>();
                if (T->isa<Arr>()) gate("tensor with array element type", T);
                auto nis_l = Lit::isa<u64>(nis);
                auto r_l   = Lit::isa<u64>(r);
                auto ax_l  = Lit::isa<u64>(ax);
                if (!nis_l || !r_l || !ax_l) {
                    gate("non-literal nis/rank/axis of %tensor.concat", app);
                } else {
                    add_tensor_ty(app->type());
                    for (u64 i = 0; i < *nis_l; ++i) {
                        // The loop generation needs literal extents along `ax` for the prefix offsets.
                        if (!Lit::isa<u64>(Sis->proj(*nis_l, i)->proj(*r_l, *ax_l)))
                            gate("non-literal extent along the concat axis", app);
                        add_tensor_ty(app->arg()->proj(*nis_l, i)->type());
                    }
                }
            } else if (auto [axm, curry, trip] = Axm::get(app);
                       axm && curry == 0 && axm->plugin() == tensor::Plugin_Id) {
                // Any other tensor op (a symbolic `shape`, …) has no buffer-world lowering.
                gate("unbufferizable tensor op", app);
            }
            // Lams passed inside a tensor op's curry chain (combiners, affine index maps) are element-level.
            if (is_tensor_op(app)) {
                for (const App* a = app; a; a = a->callee()->isa<App>()) {
                    if (auto k = a->arg()->isa_mut<Lam>()) op_args_.emplace(k);
                    for (auto op : a->arg()->ops())
                        if (auto k = op ? op->isa_mut<Lam>() : nullptr) op_args_.emplace(k);
                }
            }
        }

        for (auto op : def->ops())
            push(op);
        push(def->type());
    }

    for (auto mut : old_world().externals().muts())
        if (auto lam = mut->isa_mut<Lam>(); lam && is_tensor_fn(lam)) tensor_fns_.emplace(lam);
    // No tensor boundaries AND no tensor ops: nothing for the sweeps below to check.
    // (Ops without boundaries still bufferize: their value-world operands are materialized.)
    if (tensor_fns_.empty() && !ops_seen_) return;

    // Higher-order bufferized functions: a continuation parameter whose domain itself contains a Pi would
    // need boundary conversion inside nested continuation types, which `conv_boundary` does not perform.
    for (auto old_fn : tensor_fns_) {
        auto dom = old_fn->type()->dom();
        auto n   = dom->num_projs();
        for (size_t i = 0; i != n; ++i)
            if (auto pi = Pi::isa_cn(dom->proj(n, i)); pi && contains_pi(pi->dom()))
                return gate("higher-order bufferized function", old_fn);
    }

    // Second sweep: shapes the conversion cannot adapt — the value-semantics path lowers everything instead.
    unique_queue<DefSet> wl2;
    for (auto mut : old_world().externals().muts())
        if (mut) wl2.push(mut);
    while (!wl2.empty()) {
        auto def = wl2.pop();

        for (auto op : def->ops()) {
            if (!op) continue;
            if (auto fn = op->isa_mut<Lam>()) {
                // A bufferized function referenced as a value (not as the callee of a call, and not the
                // binder back-reference of its own variable) cannot be adapted.
                if (tensor_fns_.contains(fn))
                    if (!(def->isa<App>() && def->as<App>()->callee() == op) && !def->isa<Var>())
                        return gate("bufferized function used as a value", fn);
                if (!tensor_fns_.contains(fn) && !op_args_.contains(fn) && mentions_tensor(fn->type()->dom())) {
                    // A tensor-typed function the conversion does not rewrite: an unset external keeps its
                    // value ABI, a direct-style local cannot be rebuilt as a continuation — either way a
                    // bufferized caller would pass a buffer against a value-array signature.
                    if (fn->is_external() || !Pi::isa_cn(fn->type()))
                        return gate("unconvertible tensor-typed function", fn);
                }
            }
            wl2.push(op);
        }
        if (def->type()) wl2.push(def->type());
    }
}

void LowerToMem::start() {
    collect_tensor_types(); // hard-errors on shapes the conversion cannot handle
    // Nothing tensor-related in the program: skip the whole-world rebuild entirely.
    if (tensor_fns_.empty() && !ops_seen_) return;
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

const Def* LowerToMem::bot_mem() {
    auto& w = new_world();
    return w.bot(w.call<mem::M>(0));
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

const Def* LowerToMem::conv_boundary(const Def* t) {
    if (tensor_ty_.contains(t)) return buf_of(t);
    if (auto sig = t->isa_imm<Sigma>(); sig && mentions_tensor(sig)) {
        auto n = sig->num_ops();
        DefVec ops(n);
        for (size_t i = 0; i != n; ++i)
            ops[i] = conv_boundary(sig->op(i));
        return new_world().sigma(ops);
    }
    return rewrite(t);
}

const Def* LowerToMem::rewrite_mut_Lam(Lam* lam) {
    if (is_bootstrapping()) return RWPhase::rewrite_mut_Lam(lam);

    // A bufferized function: convert tensor-typed parameters to `%buffer.Buf`, including inside grouped
    // sigma parameters and continuation domains. No memory is introduced here — AddMem does that.
    if (is_tensor_fn(lam)) {
        auto& w  = new_world();
        auto dom = lam->type()->dom();
        auto n   = dom->num_projs();

        DefVec doms(n);
        for (size_t i = 0; i != n; ++i) {
            auto d = dom->proj(n, i);
            if (auto pi = Pi::isa_cn(d))
                doms[i] = w.cn(conv_boundary(pi->dom()));
            else
                doms[i] = conv_boundary(d);
        }

        auto new_lam = w.mut_con(doms)->set(lam->dbg());
        map(lam, new_lam);
        if (lam->num_vars() != 0) map(lam->var(), new_lam->var());
        new_lam->set(rewrite(lam->filter()), rewrite(lam->body()));
        return new_lam;
    }

    // A local continuation carrying tensor types (a return continuation of a bufferized call, a join point,
    // an error continuation): convert its domain the same way. This is context-independent, so the order in
    // which references reach it does not matter.
    if (!lam->is_external() && lam->is_set() && !op_args_.contains(lam)) {
        if (auto pi = Pi::isa_cn(lam->type()); pi && mentions_tensor(pi->dom())) {
            auto& w      = new_world();
            auto new_lam = w.mut_con(conv_boundary(pi->dom()))->set(lam->dbg());
            map(lam, new_lam);
            if (lam->num_vars() != 0) map(lam->var(), new_lam->var());
            new_lam->set(rewrite(lam->filter()), rewrite(lam->body()));
            return new_lam;
        }
    }

    return RWPhase::rewrite_mut_Lam(lam);
}

const Def* LowerToMem::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    if (Axm::isa<tensor::get>(app)) return lower_get(app);
    if (Axm::isa<tensor::set>(app)) return lower_set(app);
    if (Axm::isa<tensor::broadcast>(app)) return lower_broadcast(app);
    if (Axm::isa<tensor::map_reduce>(app)) return lower_map_reduce(app);
    if (Axm::isa<tensor::pad>(app)) return lower_pad(app);
    if (Axm::isa<tensor::concat>(app)) return lower_concat(app);

    // Call of a bufferized function: adapt the call site.
    if (auto callee = app->callee()->isa_mut<Lam>(); callee && tensor_fns_.contains(callee))
        return lower_call(app, callee);

    // Call of a converted continuation (a local lam or a parameter var whose domain mentions a tensor):
    // materialize value-world tensor arguments into buffers. Element-level lams (op_args_) keep value ABI.
    if (auto pi = Pi::isa_cn(app->callee()->type()); pi && mentions_tensor(pi->dom())) {
        if (auto callee = app->callee()->isa_mut<Lam>(); callee && op_args_.contains(callee))
            return RWPhase::rewrite_imm_App(app);
        auto& w = new_world();
        return w.app(rewrite(app->callee()), materialize(pi->dom(), app->arg()));
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LowerToMem::lower_call(const App* app, Lam* old_callee) {
    auto& w         = new_world();
    auto new_callee = rewrite(old_callee);
    auto dom        = old_callee->type()->dom();
    auto n          = dom->num_projs();

    DefVec args(n);
    for (size_t i = 0; i != n; ++i) {
        auto d = dom->proj(n, i);
        auto a = app->arg()->proj(n, i);
        // Continuations pass through: their domains are converted by `rewrite_mut_Lam` to exactly the
        // domain the callee's new signature expects.
        args[i] = Pi::isa_cn(d) ? rewrite(a) : materialize(d, a);
    }
    return w.app(new_callee, w.tuple(args));
}

const Def* LowerToMem::splat_buffer(const Def* arr_ty, const Def* scalar) {
    // `%buffer.constant` sets every element to `scalar`; `%matrix.lower_aff` fills it with a loop rather
    // than storing a monolithic literal array (which the LLVM backend cannot digest for large shapes).
    auto [bro, bso, boT] = Axm::isa<buffer::Buf>(buf_of(arr_ty))->args<3>();
    auto [m, out]        = buffer::op_constant(bro, bso, boT, bot_mem(), scalar)->projs<2>();
    return out;
}

const Def* LowerToMem::materialize(const Def* old_ty, const Def* old_arg) {
    auto& w = new_world();
    if (tensor_ty_.contains(old_ty)) {
        // A constant splat `‹s; c›` (e.g. a learning-rate or bias literal): a `%buffer.init` would store the
        // whole array as one giant LLVM constant. Emit `%buffer.constant` (lowered to a fill loop) instead.
        if (auto c = splat_scalar(old_arg)) return splat_buffer(old_ty, rewrite(c));
        auto v = rewrite(old_arg);
        if (Axm::isa<buffer::Buf>(v->type())) return v; // already a buffer
        auto [br, bs, bT] = Axm::isa<buffer::Buf>(buf_of(old_ty))->args<3>();
        auto [m, buf]     = buffer::op_init(br, bs, bT, bot_mem(), v)->projs<2>();
        return buf;
    }
    if (auto sig = old_ty->isa_imm<Sigma>(); sig && mentions_tensor(sig)) {
        auto n = sig->num_ops();
        DefVec ops(n);
        for (size_t i = 0; i != n; ++i)
            ops[i] = materialize(sig->op(i), old_arg->proj(n, i));
        return w.tuple(ops);
    }
    return rewrite(old_arg);
}

const Def* LowerToMem::lower_get(const App* app) {
    auto c            = rewrite(app->callee())->as<App>();
    auto arg          = rewrite(app->arg());
    auto [arr, index] = arg->projs<2>();
    auto [T, r, s]    = c->args<3>();
    auto buf          = Axm::isa<buffer::Buf>(arr->type());
    // A `get` on a value-world tensor (e.g. a literal): materialize it into a buffer.
    if (!buf) {
        arr = materialize(app->arg()->proj(0)->type(), app->arg()->proj(0));
        buf = Axm::isa<buffer::Buf>(arr->type());
        if (!buf) return RWPhase::rewrite_imm_App(app); // not a recorded tensor type: leave it alone
    }
    auto [br, bs, bT] = buf->args<3>(); // actual (folded) buffer metadata

    auto [m, v] = buffer::op_read(br, bs, bT, bot_mem(), arr, fold_index(s, index))->projs<2>();
    return v; // the loaded value
}

const Def* LowerToMem::lower_set(const App* app) {
    auto c               = rewrite(app->callee())->as<App>();
    auto arg             = rewrite(app->arg());
    auto [arr, index, x] = arg->projs<3>();
    auto [T, r, s]       = c->args<3>();
    auto buf             = Axm::isa<buffer::Buf>(arr->type());
    // A `set` on a value-world tensor (e.g. a literal): materialize it into a buffer.
    if (!buf) {
        arr = materialize(app->arg()->proj(0)->type(), app->arg()->proj(0));
        buf = Axm::isa<buffer::Buf>(arr->type());
        if (!buf) return RWPhase::rewrite_imm_App(app); // not a recorded tensor type: leave it alone
    }
    auto [br, bs, bT] = buf->args<3>(); // actual (folded) buffer metadata
    auto fidx         = fold_index(s, index);

    if (reuse_in_place(app)) {
        auto [m, buf2] = buffer::op_write(br, bs, bT, bot_mem(), arr, fidx, x)->projs<2>();
        return buf2;
    }

    // AlwaysAllocate policy: allocate a fresh buffer, copy the source in, then write the element.
    // This local chain is properly threaded; AddMem splices its `⊥` root into the global chain.
    auto [m1, q]   = buffer::op_alloc(br, bs, bT, bot_mem())->projs<2>();
    auto m2        = buffer::op_copy(br, bs, bT, m1, q, arr);
    auto [m3, out] = buffer::op_write(br, bs, bT, m2, q, fidx, x)->projs<2>();
    return out;
}

const Def* LowerToMem::lower_broadcast(const App* app) {
    // Thin bufferization: map the SSA `tensor.broadcast` onto the buffer-world `matrix.broadcast`.
    // The loop generation lives in the matrix plugin (`%matrix.lower_aff`).
    auto& w                   = new_world();
    auto c                    = rewrite(app->callee())->as<App>();
    auto arg                  = rewrite(app->arg());
    auto [s_in, s_out, input] = arg->projs<3>();
    auto [T, r]               = c->args<2>();

    // No-op broadcast (already normalized away in most cases).
    if (s_in == s_out) return input;

    // A broadcast of a value-world tensor (e.g. a literal): materialize it into a buffer.
    auto in_buf = Axm::isa<buffer::Buf>(input->type());
    if (!in_buf) {
        input  = materialize(app->arg()->proj(2)->type(), app->arg()->proj(2));
        in_buf = Axm::isa<buffer::Buf>(input->type());
    }

    // Rank-0 source: an all-size-1 input shape folds to a plain scalar that is never recorded as a tensor
    // type, so `materialize` leaves it as a value. Broadcasting a scalar fills every element with it.
    if (!in_buf) return splat_buffer(app->type(), input);

    // Actual (size-1-folded) input/output buffer shapes — `matrix.broadcast` is parameterised by them.
    auto [bri, bsi, biT] = in_buf->args<3>();
    auto [bro, bso, boT] = Axm::isa<buffer::Buf>(buf_of(app->type()))->args<3>();

    auto op       = w.annex<matrix::broadcast>();
    op            = w.app(op, w.tuple({T, bri, bsi, bro, bso, r}));
    op            = w.app(op, w.tuple({s_in, s_out}));
    auto [m, out] = w.app(op, w.tuple({bot_mem(), input}))->projs<2>();
    return out;
}

const Def* LowerToMem::lower_map_reduce(const App* app) {
    // Thin bufferization: map the SSA `tensor.map_reduce` onto the buffer-world `matrix.map_reduce_aff`,
    // reusing the (rewritten) meta. The loop generation lives in the matrix plugin (`%matrix.lower_aff`).
    auto& w     = new_world();
    auto c      = rewrite(app->callee())->as<App>();
    auto inputs = rewrite(app->arg()); // the (bufferized) input buffers `is`

    auto [nis, meta, shapes, TisRisSis, comb_init, acc_out, accs] = c->uncurry_args<7>();
    auto [comb, init]                                             = comb_init->projs<2>();

    // Value-world tensor inputs (e.g. literals): materialize them into buffers.
    if (auto nis_l = Lit::isa<u64>(nis)) {
        DefVec ins(*nis_l);
        for (u64 i = 0; i < *nis_l; ++i) {
            ins[i] = inputs->proj(*nis_l, i);
            if (!Axm::isa<buffer::Buf>(ins[i]->type())) {
                auto old_in = app->arg()->proj(*nis_l, i);
                ins[i]      = materialize(old_in->type(), old_in);
                if (!Axm::isa<buffer::Buf>(ins[i]->type()))
                    return RWPhase::rewrite_imm_App(app); // not a recorded tensor type: leave it alone
            }
        }
        // Re-tuple the inputs: the generic rewrite rebuilds the argument tuple with its stale value-array
        // type even when its elements were converted to buffers, which would not be assignable to the op's
        // `«nis; %buffer.Buf …»` domain.
        inputs = w.tuple(ins);
    }

    // Wrap the pure tensor combiner `Fn [To, «nis; Tis»] → To` into the mem-threaded combiner
    // `Fn [%mem.M 0, To, «nis; Tis»] → [%mem.M 0, To]` that `matrix.map_reduce_aff` expects.
    auto mem_ty           = w.call<mem::M>(0);
    auto inner            = comb->type()->as<Pi>()->dom()->proj(0); // [To, «nis; Tis»]
    auto [cTo, ins_ty]    = inner->projs<2>();
    auto memcomb          = w.mut_fun(w.sigma({mem_ty, cTo, ins_ty}), w.sigma({mem_ty, cTo}))->set("memComb");
    auto [cm, cacc, cins] = memcomb->var(0_n)->projs<3>();
    auto cret             = memcomb->var(1);
    auto after            = w.mut_con(cTo)->set("afterComb");
    after->app(true, cret, w.tuple({cm, after->var(0_n)}));
    memcomb->set(true, w.app(comb, w.tuple({w.tuple({cacc, cins}), after})));

    auto op       = w.annex<matrix::map_reduce_aff>();
    op            = w.app(op, nis);
    op            = w.app(op, meta);
    op            = w.app(op, shapes);
    op            = w.app(op, TisRisSis);
    op            = w.app(op, w.tuple({memcomb, init}));
    op            = w.app(op, acc_out);
    op            = w.app(op, accs);
    auto [m, out] = w.app(op, w.tuple({bot_mem(), inputs}))->projs<2>();
    return out;
}

const Def* LowerToMem::lower_pad(const App* app) {
    // Thin bufferization: map the SSA `tensor.pad` onto the buffer-world `matrix.pad`.
    // The loop generation lives in the matrix plugin (`%matrix.lower_aff`).
    auto& w             = new_world();
    auto c              = rewrite(app->callee())->as<App>();
    auto [input, value] = rewrite(app->arg())->projs<2>();

    auto [Tr, s_in, params] = c->uncurry_args<3>();
    auto [T, r]             = Tr->projs<2>();
    auto [mode, lo, hi]     = params->projs<3>();

    // A pad of a value-world tensor (e.g. a literal): materialize it into a buffer.
    if (!Axm::isa<buffer::Buf>(input->type())) {
        input = materialize(app->arg()->proj(0)->type(), app->arg()->proj(0));
        if (!Axm::isa<buffer::Buf>(input->type()))
            return RWPhase::rewrite_imm_App(app); // not a recorded tensor type: leave it alone
    }

    auto r_l = Lit::isa<u64>(r);
    if (!r_l) return RWPhase::rewrite_imm_App(app);

    // The LOGICAL output shape `s_out#d = lo#d + s_in#d + hi#d` — the loop generation iterates it, so it
    // must keep size-1 axes (the result type's `Buf` folds them away and cannot be used here).
    DefVec so(*r_l);
    for (u64 d = 0; d < *r_l; ++d)
        so[d] = w.call(core::nat::add, DefVec{w.call(core::nat::add, DefVec{lo->proj(*r_l, d), s_in->proj(*r_l, d)}),
                                              hi->proj(*r_l, d)});
    auto s_out = w.tuple(so);

    auto op       = w.annex<matrix::pad>();
    op            = w.app(op, w.tuple({T, r}));
    op            = w.app(op, w.tuple({s_in, s_out, mode, lo, hi}));
    auto [m, out] = w.app(op, w.tuple({bot_mem(), input, value}))->projs<2>();
    return out;
}

const Def* LowerToMem::lower_concat(const App* app) {
    // Thin bufferization: map the SSA `tensor.concat` onto the buffer-world `matrix.concat`.
    // The loop generation lives in the matrix plugin (`%matrix.lower_aff`).
    auto& w  = new_world();
    auto c   = rewrite(app->callee())->as<App>();
    auto arg = rewrite(app->arg());

    auto [TnisR, ax, Sis] = c->uncurry_args<3>();
    auto [T, nis, r]      = TnisR->projs<3>();

    auto nis_l = Lit::isa<u64>(nis);
    auto r_l   = Lit::isa<u64>(r);
    auto ax_l  = Lit::isa<u64>(ax);
    if (!nis_l || !r_l || !ax_l) return RWPhase::rewrite_imm_App(app);

    // Value-world tensor inputs (e.g. literals): materialize them into buffers. Re-tuple the projected
    // inputs so the tuple type is re-inferred from the converted elements (see `lower_map_reduce`).
    DefVec ins(*nis_l);
    for (u64 i = 0; i < *nis_l; ++i) {
        ins[i] = arg->proj(*nis_l, i);
        if (!Axm::isa<buffer::Buf>(ins[i]->type())) {
            auto old_in = app->arg()->proj(*nis_l, i);
            ins[i]      = materialize(old_in->type(), old_in);
            if (!Axm::isa<buffer::Buf>(ins[i]->type()))
                return RWPhase::rewrite_imm_App(app); // not a recorded tensor type: leave it alone
        }
    }
    auto inputs = w.tuple(ins);

    // The LOGICAL output shape: the summed extent along `ax` (literal, gated in `collect_tensor_types`),
    // the shared extents elsewhere — the loop generation iterates it, so it must keep size-1 axes (the
    // result type's `Buf` folds them away and cannot be used here).
    u64 sum_ax = 0;
    for (u64 i = 0; i < *nis_l; ++i) {
        auto e = Lit::isa<u64>(Sis->proj(*nis_l, i)->proj(*r_l, *ax_l));
        if (!e) return RWPhase::rewrite_imm_App(app);
        sum_ax += *e;
    }
    DefVec so(*r_l);
    for (u64 d = 0; d < *r_l; ++d)
        so[d] = d == *ax_l ? w.lit_nat(sum_ax) : Sis->proj(*nis_l, 0)->proj(*r_l, d);
    auto s_out = w.tuple(so);

    auto op       = w.annex<matrix::concat>();
    op            = w.app(op, w.tuple({T, nis, r}));
    op            = w.app(op, ax);
    op            = w.app(op, Sis);
    op            = w.app(op, s_out);
    auto [m, out] = w.app(op, w.tuple({bot_mem(), inputs}))->projs<2>();
    return out;
}

} // namespace mim::plug::tensor::phase
