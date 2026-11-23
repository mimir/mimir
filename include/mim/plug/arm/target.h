#pragma once

#include <mim/target.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

/// The machine facts of the AArch64 backend; the ABI is AAPCS64.
class Target : public mim::Target {
public:
    Target();

    Class cls(const Def* def) const final;
    bool is_machine_insn(const App* app) const final;
    bool is_mem_insn(const App* app) const final {
        return Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<arm::slot>(app);
    }
    bool is_in(const Def* def) const final { return Axm::isa<arm::in>(def); }
    const Def* in(Class cls, size_t r, const Def* v) const final;
    nat_t ptr_size() const final { return 8; }

    size_t num_regs(Class cls) const final { return cls == Gpr ? Num_Regs : 0; }
    bool reserved(Class, size_t r) const final { return arm::reserved(r); }
    bool caller_saved(Class, size_t r) const final { return Caller_Saved[r]; }
    size_t num_arg_regs(Class cls) const final { return cls == Gpr ? Arg_Regs.size() : 0; }
    size_t arg_reg(Class, size_t i) const final { return Arg_Regs[i]; }
    size_t ret_reg(Class) const final { return Ret_Reg; }

    bool is_removable(const App* insn) const final;
    const Def* spill(Class cls, const Def* type) const final;
    const Def* reload(Class cls, const Def* type) const final;
    const Def* spill_type(Class cls, const Def* type) const final;
};

const Target& target();

} // namespace mim::plug::arm
