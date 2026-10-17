#include "mim/plug/btensor/phase/free_locals.h"

#include <mim/plug/buffer/buffer.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/btensor/btensor.h"

namespace mim::plug::btensor::phase {

bool FreeLocals::Escape::allocates(const App* app) const {
    return Axm::isa<buffer::alloc>(app) || Axm::isa<buffer::init>(app) || Axm::isa<buffer::lit>(app);
}

/// Every op of this plugin takes its `dst` right after the memory and returns it.
const Def* FreeLocals::Escape::hands_back(const App* app) const {
    auto [axm, curry, _] = Axm::get(app);
    if (Axm::isa<buffer::write>(app) || (axm && curry == 0 && axm->plugin() == btensor::Plugin_Id))
        return app->arg()->proj(app->arg()->num_projs(), 1);
    return nullptr;
}

bool FreeLocals::Escape::is_handle(const Def* type) const { return Axm::isa<buffer::Buf>(type); }

bool FreeLocals::analyze() {
    if (RWPhase::analyze()) return true;

    for (const auto& [lam, chain] : escape_.chains()) {
        auto last = DefMap<size_t>();
        for (size_t i = 0, n = chain.ops.size(); i != n; ++i) {
            last[chain.ops[i]] = i;
            for (auto alloc : chain.uses[i])
                last[alloc] = i;
        }

        for (auto alloc : chain.locals)
            if (escape_.confined(alloc)) {
                auto i = last[alloc] + 1;
                frees_[i < chain.ops.size() ? chain.ops[i] : chain.tail].emplace_back(alloc);
            }

        for (auto scope : chain.leaves)
            for (auto alloc : escape_.locals(scope))
                if (!escape_.confined(alloc)) frees_[chain.tail].emplace_back(alloc);
    }

    log().d("free_locals: frees before {} ops", frees_.size());
    return false;
}

const Def* FreeLocals::rewrite_imm_App(const App* app) {
    if (auto allocs = fe::lookup(frees_, app)) {
        if (auto mem = mem::mem_def(app->arg())) {
            auto new_arg = thread(app->arg(), mem, *allocs);
            return new_world().app(rewrite(app->callee()), new_arg);
        }
        log().d("free_locals: `{}` threads no memory, leaking {} buffers", app, allocs->size());
    }
    return RWPhase::rewrite_imm_App(app);
}

const Def* FreeLocals::thread(const Def* arg, const Def* mem, const DefVec& allocs) {
    if (arg == mem) {
        auto new_mem = rewrite(mem);
        for (auto alloc : allocs)
            new_mem = buffer::op_free(new_mem, rewrite(alloc->proj(2, 1)));
        return new_mem;
    }
    if (mem::mem_def(arg) != mem) return rewrite(arg);
    auto n = arg->num_projs();
    return new_world().tuple(DefVec(n, [&](size_t i) { return thread(arg->proj(n, i), mem, allocs); }));
}

} // namespace mim::plug::btensor::phase
