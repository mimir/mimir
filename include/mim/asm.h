#pragma once

#include <deque>
#include <ostream>
#include <string>

#include "mim/phase.h"

namespace mim {

/// The `emit` Phase of a native backend: writes GNU assembler code for each external function and each function it
/// calls. The output goes to `<world>.s` (or `a.s` if the World is unnamed), or to `<file>` of `-X <plugin>:o=<file>`.
class AsmEmit : public Phase {
public:
    AsmEmit(World& world, flags_t annex, std::string_view backend)
        : Phase(world, annex)
        , backend_(backend) {}

    void start() final;

    /// The assembler symbol of @p lam; queues an internal function for emission.
    std::string symbol(Lam* lam);

protected:
    virtual void emit(std::ostream& os, Lam* fun, bool global) = 0;
    /// Writes what all functions together need after them, e.g. a constant pool.
    virtual void finish(std::ostream&) {}

private:
    std::string_view backend_;
    std::deque<Lam*> todo_;
    LamSet seen_;
};

} // namespace mim
