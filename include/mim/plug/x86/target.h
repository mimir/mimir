#pragma once

#include <mim/target.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// The machine facts of the x86-64 backend; the ABI is System V AMD64.
class Target : public mim::Target {
public:
    Target();

    Class cls(const Def* def) const final;
    bool is_machine_insn(const App* app) const final;
    bool is_mem_insn(const App* app) const final;
    bool is_in(const Def* def) const final { return Axm::isa<x86::in>(def) || Axm::isa<fin>(def); }
    const Def* in(Class cls, size_t r, const Def* v) const final;
    nat_t ptr_size() const final { return 8; }

    size_t num_regs(Class cls) const final { return cls == Gpr ? Num_Regs : Num_Xmm; }
    bool reserved(Class cls, size_t r) const final { return cls == Gpr && x86::reserved(r); }
    /// The XMM registers are all caller-saved.
    bool caller_saved(Class cls, size_t r) const final { return cls != Gpr || Caller_Saved[r]; }
    /// The XMM registers are their own argument registers.
    size_t num_arg_regs(Class cls) const final { return cls == Gpr ? Arg_Regs.size() : 8; }
    size_t arg_reg(Class cls, size_t i) const final { return cls == Gpr ? Arg_Regs[i] : i; }
    size_t ret_reg(Class cls) const final { return cls == Gpr ? encoding(reg::rax) : 0; }

    Constraint constraint(const App* insn) const final;
    bool is_removable(const App* insn) const final;
    const Def* spill(Class cls, const Def* type) const final;
    const Def* reload(Class cls, const Def* type) const final;
    const Def* spill_type(Class cls, const Def* type) const final;
};

const Target& target();

} // namespace mim::plug::x86
