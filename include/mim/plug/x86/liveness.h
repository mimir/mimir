#pragma once

#include <mim/liveness.h>

#include "mim/plug/x86/target.h"
#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

class Liveness : public mim::Liveness {
public:
    explicit Liveness(Lam* fun)
        : mim::Liveness(x86::target(), fun) {}

    static Class cls(const Def* value) { return x86::target().cls(value); }
};

/// Is @p value a value of class @p cls?
inline bool is_value(const Def* value, Liveness::Class cls) { return mim::is_value(value, cls, target()); }

/// System V AMD64 ABI per register class: the XMM registers are their own argument registers and all caller-saved.
inline size_t num_arg_regs(Liveness::Class cls) { return cls == Liveness::Gpr ? Arg_Regs.size() : 8; }
inline size_t arg_reg(Liveness::Class cls, size_t i) { return cls == Liveness::Gpr ? Arg_Regs[i] : i; }
inline size_t ret_reg(Liveness::Class cls) { return cls == Liveness::Gpr ? encoding(reg::rax) : 0; }
inline bool caller_saved(Liveness::Class cls, size_t r) { return cls != Liveness::Gpr || Caller_Saved[r]; }

} // namespace mim::plug::x86
