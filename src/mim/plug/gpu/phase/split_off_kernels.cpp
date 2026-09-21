#include "mim/plug/gpu/phase/split_off_kernels.h"

#include <mim/driver.h>

namespace mim::plug::gpu::phase {

void SplitOffKernels::start() {
    analyze();

    for (const auto& [f, entry] : old_world().annexes())
        rewrite_annex(f, entry.sym, entry.def);

    for (auto kernel : kernels_)
        rewrite(kernel);
}

bool SplitOffKernels::analyze() {
    for (auto def : old_world().annexes().defs())
        analyze(def);
    for (auto def : old_world().externals().muts())
        analyze(def);

    return false; // no fixed-point necessary
}

void SplitOffKernels::analyze(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    if (auto launch = Axm::isa<gpu::launch>(def)) {
        auto kernel = launch->decurry()->decurry()->arg();
        if (auto lam = kernel->isa_mut<Lam>()) kernels_.emplace(lam);
    }

    for (auto d : def->deps())
        analyze(d);
}

const Def* SplitOffKernels::rewrite_mut_Lam(Lam* old_lam) {
    auto new_def = RWPhase::rewrite_mut_Lam(old_lam);

    if (kernels_.contains(old_lam)) {
        // A kernel's name becomes its external symbol, so gid-suffix it: two kernels may share a name.
        old_lam->set<true>(old_lam->unique_name());
        new_def->as_mut<Lam>()->set<true>(old_lam->sym())->externalize();
        old_lam->unset();
    }

    return new_def;
}

} // namespace mim::plug::gpu::phase
