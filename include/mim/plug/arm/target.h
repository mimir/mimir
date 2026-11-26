#pragma once

#include <mim/plug/mach/target.h>

#include "mim/plug/arm/arm.h"

namespace mim::plug::arm {

/// The machine facts of the AArch64 backend; the ABI is AAPCS64.
class Target : public mach::Target {
public:
    Target()
        : mach::Target("arm", Plugin_Id) {}

    bool is_flags(const Def* def) const final { return Axm::isa<mach::Flags>(def->type()) || Axm::isa<cc>(def); }
    bool is_sched_cc(const App* app) const final {
        return Axm::isa<mach::sched>(app) && Axm::isa<cc, 1>(sched_insn(app));
    }

    bool is_mem_insn(const App* app) const final {
        return mach::Target::is_mem_insn(app) || Axm::isa<load>(app) || Axm::isa<store>(app) || Axm::isa<fload>(app)
            || Axm::isa<fstore>(app);
    }

    size_t num_regs(Class cls) const final { return cls == Gpr ? Num_Regs : cls == Fpr ? Num_Fpr : 0; }
    bool reserved(Class cls, size_t r) const final { return cls == Fpr ? r == Fpr_Scratch : arm::reserved(r); }
    bool caller_saved(Class cls, size_t r) const final {
        return cls == Gpr ? Caller_Saved[r] : arm::caller_saved_fpr(r);
    }
    size_t num_arg_regs(Class cls) const final {
        return cls == Gpr ? Arg_Regs.size() : cls == Fpr ? Fpr_Arg_Regs.size() : 0;
    }
    size_t arg_reg(Class cls, size_t i) const final { return cls == Gpr ? Arg_Regs[i] : Fpr_Arg_Regs[i]; }
    size_t ret_reg(Class cls) const final { return cls == Gpr ? Ret_Reg : Ret_Fpr; }

    bool is_removable(const App* insn) const final {
        return mach::Target::is_removable(insn) || Axm::isa<load>(insn) || Axm::isa<fload>(insn);
    }
};

} // namespace mim::plug::arm
