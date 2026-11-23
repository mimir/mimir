#pragma once

#include <algorithm>
#include <deque>
#include <ostream>
#include <print>
#include <string>

#include <mim/phase.h>
#include <mim/plugin.h>
#include <mim/world.h>

#include "mim/plug/mach/target.h"

namespace mim::plug::mach {

/// The `emit` Phase of a native backend: writes GNU assembler code for each external function and each function it
/// calls. The output goes to `<world>.s` (or `a.s` if the World is unnamed), or to `<file>` of `-X <plugin>:o=<file>`.
class AsmEmit : public Phase {
public:
    AsmEmit(World& world, flags_t annex, const Target& target)
        : Phase(world, annex)
        , target_(target) {}

    void start() final {
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
        if (Target::is_elf()) std::println(os, "\t.section .note.GNU-stack,\"\",@progbits");
    }

    /// The assembler symbol of @p lam; queues an internal function for emission.
    std::string symbol(Lam* lam) {
        if (lam->is_external() || !lam->is_set()) return std::format("{}{}", Target::global_prefix(), lam->sym());
        if (seen_.emplace(lam).second) todo_.emplace_back(lam);
        return lam->unique_name();
    }

protected:
    virtual void emit(std::ostream& os, Lam* fun, bool global) = 0;
    /// Writes what all functions together need after them, e.g. a constant pool.
    virtual void finish(std::ostream&) {}

private:
    const Target& target_;
    std::deque<Lam*> todo_;
    LamSet seen_;
};

} // namespace mim::plug::mach
