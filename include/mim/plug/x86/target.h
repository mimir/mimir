#pragma once

#include <mim/target.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

/// The machine facts of the x86-64 backend.
class Target : public mim::Target {
public:
    Target();

    Class cls(const Def* def) const final;
    bool is_machine_insn(const App* app) const final;
    bool is_mem_insn(const App* app) const final;
    bool is_in(const Def* def) const final { return Axm::isa<x86::in>(def) || Axm::isa<fin>(def); }
    const Def* in(Class cls, size_t r, const Def* v) const final;
    nat_t ptr_size() const final { return 8; }
};

const Target& target();

} // namespace mim::plug::x86
