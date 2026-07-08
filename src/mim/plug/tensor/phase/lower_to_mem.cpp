#include "mim/plug/tensor/phase/lower_to_mem.h"

#include "mim/axm.h"
#include "mim/def.h"
#include "mim/lam.h"

#include "mim/plug/buffer/buffer.h"
#include "mim/plug/matrix/matrix.h"
#include "mim/plug/mem/mem.h"
#include "mim/plug/tensor/phase/add_mem_buf.h"
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

/// Is `app` one of the tensor ops this phase bufferizes?
bool is_tensor_op(const App* app) {
    return Axm::isa<tensor::get>(app) || Axm::isa<tensor::set>(app) || Axm::isa<tensor::broadcast>(app)
        || Axm::isa<tensor::map_reduce>(app);
}

} // namespace

void LowerToMem::collect_tensor_types() {
    auto gate = [this](const char* why, const Def* culprit) {
        if (bufferize_) WLOG("bufferization disabled: {} ({})", why, culprit);
        bufferize_ = false;
    };
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
                // Array types fold literal size-1 axes but `%buffer.Buf` does not, so the buffer types the
                // `%matrix.map_reduce_aff` axiom derives from these logical shapes would mismatch the folded
                // boundary types.
                auto has_size1 = [](const Def* s) {
                    auto n = s->num_projs();
                    for (size_t i = 0; i != n; ++i)
                        if (auto l = Lit::isa<u64>(s->proj(n, i)); l && *l == 1) return true;
                    return false;
                };
                if (has_size1(shapes->proj(2, 0))) gate("size-1 output axis of %tensor.map_reduce", app);
                if (auto nis_l = Lit::isa<u64>(nis)) {
                    auto [Tis, Ris, Sis] = TisRisSis->projs<3>();
                    for (u64 i = 0; i < *nis_l; ++i) {
                        if (Tis->proj(*nis_l, i)->isa<Arr>()) gate("tensor with array element type", Tis);
                        if (has_size1(Sis->proj(*nis_l, i)))
                            gate("size-1 input axis of %tensor.map_reduce", app);
                        add_tensor_ty(app->arg()->proj(*nis_l, i)->type());
                    }
                }
            } else if (Axm::isa<tensor::broadcast>(app)) {
                // result «s_out; T» and input «s_in; T» (the 3rd argument) are tensors.
                auto [T, r] = app->callee()->as<App>()->args<2>();
                if (T->isa<Arr>()) gate("tensor with array element type", T);
                add_tensor_ty(app->type());
                add_tensor_ty(app->arg()->proj(2)->type());
            } else if (auto [axm, curry, trip] = Axm::get(app); axm && curry == 0
                                                                && axm->plugin() == tensor::Plugin_Id) {
                // Any other tensor op (`pad`, `concat`, a symbolic `shape`, …) is only lowered by the
                // value-semantics path, which runs after this phase — bufferizing around it would mix the
                // two worlds on interlinked tensors and be ill-typed.
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

    if (!bufferize_) return;

    for (auto mut : old_world().externals().muts())
        if (auto lam = mut->isa_mut<Lam>(); lam && is_tensor_fn(lam)) tensor_fns_.emplace(lam);
    if (tensor_fns_.empty()) return;

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
    collect_tensor_types();
    if (!bufferize_) { // disable boundary conversion: `is_tensor_fn` is then false everywhere
        tensor_ty_.clear();
        tensor_fns_.clear();
    }
    // Nothing to bufferize (value-semantics program, or gate disabled): skip the whole-world rebuild entirely.
    if (tensor_fns_.empty()) return;
    RWPhase::start();
    // Thread the memory monad through the converted world (RWPhase::start swapped it into `old_world`):
    // mem-extend continuations and replace the `⊥` memory placeholders of the emitted buffer operations
    // with the scheduler-placed current memory.
    AddMemBuf(old_world()).run();
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
    // sigma parameters and continuation domains. No memory is introduced here — AddMemBuf does that.
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
    if (!tensor_fns_.empty() && !lam->is_external() && lam->is_set() && !op_args_.contains(lam)) {
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
    if (is_bootstrapping() || !bufferize_) return RWPhase::rewrite_imm_App(app);
    if (Axm::isa<tensor::get>(app)) return lower_get(app);
    if (Axm::isa<tensor::set>(app)) return lower_set(app);
    if (Axm::isa<tensor::broadcast>(app)) return lower_broadcast(app);
    if (Axm::isa<tensor::map_reduce>(app)) return lower_map_reduce(app);

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

const Def* LowerToMem::materialize(const Def* old_ty, const Def* old_arg) {
    auto& w = new_world();
    if (tensor_ty_.contains(old_ty)) {
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
    // A `get` on a value-world tensor (e.g. a literal): leave it to the value path.
    if (!buf) return RWPhase::rewrite_imm_App(app);
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
    // A `set` on a value-world tensor (e.g. a literal): leave it to the value path.
    if (!buf) return RWPhase::rewrite_imm_App(app);
    auto [br, bs, bT] = buf->args<3>(); // actual (folded) buffer metadata
    auto fidx         = fold_index(s, index);

    if (reuse_in_place(app)) {
        auto [m, buf2] = buffer::op_write(br, bs, bT, bot_mem(), arr, fidx, x)->projs<2>();
        return buf2;
    }

    // AlwaysAllocate policy: allocate a fresh buffer, copy the source in, then write the element.
    // This local chain is properly threaded; AddMemBuf splices its `⊥` root into the global chain.
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

    // A broadcast of a value-world tensor (e.g. a literal): leave it to the value path.
    auto in_buf = Axm::isa<buffer::Buf>(input->type());
    if (!in_buf) return RWPhase::rewrite_imm_App(app);

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

    // Value-world tensor inputs (e.g. literals): leave them to the value path.
    if (auto nis_l = Lit::isa<u64>(nis)) {
        DefVec ins(*nis_l);
        for (u64 i = 0; i < *nis_l; ++i) {
            ins[i] = inputs->proj(*nis_l, i);
            if (!Axm::isa<buffer::Buf>(ins[i]->type())) return RWPhase::rewrite_imm_App(app);
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

} // namespace mim::plug::tensor::phase
