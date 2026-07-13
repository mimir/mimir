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
    for (auto& [f, entry] : old_world().annexes())
        analyze(entry.def);
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
        auto new_lam = new_def->as_mut<Lam>();
        if (new_lam->sym().empty()) {
            assert(!old_lam->sym().empty());
            new_lam->set(old_lam->sym());
        }
        new_lam->externalize();
        old_lam->unset();
    }

    return new_def;
}

} // namespace mim::plug::gpu::phase
