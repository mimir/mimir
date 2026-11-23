#pragma once

#include <mim/plug/mach/target.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

/// The machine facts of the AArch64 backend; the ABI is AAPCS64.
class Target : public mach::Target {
public:
    Target()
        : mach::Target("arm", Plugin_Id, Annex::Base<cc>) {}

    bool is_mem_insn(const App* app) const final {
        return mach::Target::is_mem_insn(app) || Axm::isa<load>(app) || Axm::isa<store>(app);
    }

    size_t num_regs(Class cls) const final { return cls == Gpr ? Num_Regs : 0; }
    bool reserved(Class, size_t r) const final { return arm::reserved(r); }
    bool caller_saved(Class, size_t r) const final { return Caller_Saved[r]; }
    size_t num_arg_regs(Class cls) const final { return cls == Gpr ? Arg_Regs.size() : 0; }
    size_t arg_reg(Class, size_t i) const final { return Arg_Regs[i]; }
    size_t ret_reg(Class) const final { return Ret_Reg; }

    bool is_removable(const App* insn) const final { return mach::Target::is_removable(insn) || Axm::isa<load>(insn); }
};

inline const Target& target() {
    static const auto res = Target();
    return res;
}

} // namespace mim::plug::arm
