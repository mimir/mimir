#include "mim/plug/gpu/phase/split_off_kernels.h"

namespace mim::plug::gpu::phase {

void SplitOffKernels::start() {
    analyze();
    if (kernels_.empty()) return;

    auto device = RWPhase(old_world(), "split_off_kernels_device");
    // A kernel's name becomes its external symbol, so gid-suffix it: two kernels may share a name.
    for (auto kernel : kernels_)
        device.rewrite(kernel)->as_mut<Lam>()->set<true>(kernel->unique_name())->externalize();

    RWPhase::start();
    swap(new_world(), device.new_world());
}

bool SplitOffKernels::analyze() {
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
    if (kernels_.contains(old_lam))
        return map(old_lam, new_world().mut_lam(rewrite(old_lam->type())->as<Pi>())->set<true>(old_lam->unique_name()));
    return RWPhase::rewrite_mut_Lam(old_lam);
}

} // namespace mim::plug::gpu::phase
