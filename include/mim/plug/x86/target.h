#pragma once

#include <mim/plug/mach/target.h>

#include "mim/plug/x86/imm.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// The machine facts of the x86-64 backend; the ABI is System V AMD64.
class Target : public mach::Target {
public:
    Target()
        : mach::Target("x86", Plugin_Id) {}

    bool is_flags(const Def* def) const final { return Axm::isa<mach::Flags>(def->type()) || Axm::isa<cc>(def); }
    bool is_sched_cc(const App* app) const final {
        return Axm::isa<mach::sched>(app) && Axm::isa<cc, 1>(sched_insn(app));
    }

    /// An address is no instruction.
    bool is_machine_insn(const App* app) const final {
        return mach::Target::is_machine_insn(app) && !Axm::isa<Addr>(app->type());
    }
    bool is_mem_insn(const App* app) const final;

    size_t num_regs(Class cls) const final { return cls == Gpr ? Num_Regs : Num_Xmm; }
    bool reserved(Class cls, size_t r) const final { return cls == Gpr && x86::reserved(r); }
    /// The XMM registers are all caller-saved.
    bool caller_saved(Class cls, size_t r) const final { return cls != Gpr || Caller_Saved[r]; }
    /// The XMM registers are their own argument registers.
    size_t num_arg_regs(Class cls) const final { return cls == Gpr ? Arg_Regs.size() : 8; }
    size_t arg_reg(Class cls, size_t i) const final { return cls == Gpr ? Arg_Regs[i] : i; }
    size_t ret_reg(Class cls) const final { return cls == Gpr ? encoding(reg::rax) : 0; }

    mach::Constraint constraint(const App* insn) const final;
    /// A literal that fits a sign-extended 32-bit immediate.
    bool fits_imm(const Def* lit) const final { return x86::fits_imm(lit); }
    bool is_removable(const App* insn) const final;
};

} // namespace mim::plug::x86
