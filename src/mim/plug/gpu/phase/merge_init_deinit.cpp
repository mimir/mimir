#include "mim/plug/gpu/phase/merge_init_deinit.h"

#include <mim/lam.h>
#include <mim/plugin.h>
#include <mim/tuple.h>

#include <mim/plug/mem/mem.h>

namespace mim::plug::gpu::phase {

namespace {

/// Collects every `%gpu.init`/`%gpu.deinit` reachable from @p def (mirrors `HostEmitter::find_kernels`'s
/// own reachability walk in the `ll_nvptx` backend).
void find_gpu_ops(const Def* def, DefSet& seen, DefVec& inits, DefVec& deinits) {
    if (auto [_, ins] = seen.emplace(def); !ins) return;
    for (auto d : def->deps())
        find_gpu_ops(d, seen, inits, deinits);
    if (Axm::isa<gpu::init>(def))
        inits.emplace_back(def);
    else if (Axm::isa<gpu::deinit>(def))
        deinits.emplace_back(def);
}

} // namespace

void MergeInitDeinit::start() {
    DefSet handled;
    for (auto old_mut : Vector<Def*>(old_world().externals().muts().begin(), old_world().externals().muts().end())) {
        auto ext = old_mut->isa_mut<Lam>();
        if (!ext || !ext->ret_pi()) continue;

        DefSet seen;
        DefVec inits, deinits;
        find_gpu_ops(ext, seen, inits, deinits);
        if (inits.size() + deinits.size() < 2) continue; // nothing to hoist

        // A shared helper reachable from more than one external: leave it to whichever external claims
        // it first, rather than hoisting it twice into two different sessions.
        if (std::ranges::any_of(inits, [&](auto d) { return handled.contains(d); })
            || std::ranges::any_of(deinits, [&](auto d) { return handled.contains(d); }))
            continue;
        for (auto d : inits)
            handled.emplace(d);
        for (auto d : deinits)
            handled.emplace(d);

        sessions_.push_back(std::make_unique<Session>());
        auto* session = sessions_.back().get();
        for (auto d : inits)
            init_session_[d] = session;
        for (auto d : deinits)
            deinit_session_[d] = session;
        ret_session_[ext->ret_var()] = session;
    }

    Super::start();
}

const Def* MergeInitDeinit::rewrite_imm_App(const App* app) {
    auto& w = new_world();

    if (auto it = deinit_session_.find(app); it != deinit_session_.end()) {
        // Eliminated unconditionally: the real teardown always happens at the function's own return
        // points instead (see the ret_session_ case below), regardless of where this occurrence sat.
        // Its own (global, const) still becomes the session's new current pair rather than being
        // discarded outright: %gpu.free calls for this occurrence's device buffers are threaded through
        // global on their way into this very call, and dropping the value would silently drop them too,
        // since nothing else would keep them reachable.
        auto* session                         = it->second;
        auto [new_mem, new_global, new_const] = rewrite(app->arg())->projs<3>();
        session->global                       = new_global;
        session->const_                       = new_const;
        return new_mem;
    }

    if (auto it = init_session_.find(app); it != init_session_.end()) {
        auto* session = it->second;
        if (!session->global) {
            // First `%gpu.init` for this function that the rewrite reaches: keep it real and cache its
            // (GlobalM, ConstM) for every later occurrence to reuse.
            auto real                         = Super::rewrite_imm_App(app);
            auto [_, real_global, real_const] = real->projs<3>();
            session->global                   = real_global;
            session->const_                   = real_const;
            return real;
        }
        return w.tuple({rewrite(app->arg()), session->global, session->const_});
    }

    if (auto it = ret_session_.find(app->callee()); it != ret_session_.end()) {
        auto* session = it->second;
        auto new_arg  = rewrite(app->arg());
        auto n        = new_arg->num_projs();
        auto mem_in   = new_arg->proj(n, 0);

        auto deinit_mem = w.app(w.annex<gpu::deinit>(), w.tuple({mem_in, session->global, session->const_}));

        DefVec parts(n);
        for (nat_t i = 0; i != n; ++i)
            parts[i] = i == 0 ? deinit_mem : new_arg->proj(n, i);

        return w.app(rewrite(app->callee()), w.tuple(parts));
    }

    return Super::rewrite_imm_App(app);
}

} // namespace mim::plug::gpu::phase
