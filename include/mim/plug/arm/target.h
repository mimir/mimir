#pragma once

#include <mim/target.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

/// The machine facts of the AArch64 backend.
class Target : public mim::Target {
public:
    Target();

    Class cls(const Def* def) const final { return Axm::isa<arm::Flags>(def->type()) ? Flags : Gpr; }
    bool is_machine_insn(const App* app) const final;
    bool is_mem_insn(const App* app) const final {
        return Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<arm::slot>(app);
    }
    bool is_in(const Def* def) const final { return Axm::isa<arm::in>(def); }
    const Def* in(Class cls, size_t r, const Def* v) const final;
    nat_t ptr_size() const final { return 8; }
};

const Target& target();

} // namespace mim::plug::arm
