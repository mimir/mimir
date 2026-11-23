#include "mim/asm.h"

#include <print>

#include "mim/plugin.h"
#include "mim/world.h"

namespace mim {

std::string AsmEmit::symbol(Lam* lam) {
    if (lam->is_external() || !lam->is_set()) return std::format("{}{}", target_.global_prefix(), lam->sym());
    if (seen_.emplace(lam).second) todo_.emplace_back(lam);
    return lam->unique_name();
}

void AsmEmit::start() {
    auto path = std::string(arg_value(args(), "o", "output").value_or(""));
    if (path.empty()) path = (world().name() ? world().name().str() : std::string("a")) + ".s";
    auto out = Out(path);
    auto& os = *out.os();

    auto muts = world().externals().mutate();
    std::ranges::sort(muts, {}, [](Def* mut) { return mut->sym().str(); });

    std::println(os, "\t.text");
    for (auto mut : muts) {
        auto lam = mut->isa<Lam>();
        if (!lam) mut->blame("{} backend: cannot emit `{}`", target_.name(), mut).bail();
        if (lam->is_set() && seen_.emplace(lam).second) emit(os, lam, true);
    }
    while (!todo_.empty()) {
        auto lam = todo_.front();
        todo_.pop_front();
        emit(os, lam, false);
    }
    finish(os);
    if (target_.is_elf()) std::println(os, "\t.section .note.GNU-stack,\"\",@progbits");
}

} // namespace mim
