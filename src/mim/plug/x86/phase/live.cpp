#include "mim/plug/x86/phase/live.h"

#include <print>

#include <mim/plugin.h>

#include "mim/plug/x86/liveness.h"

namespace mim::plug::x86::phase {

namespace {

std::string str(const DefSet& values) {
    auto sorted = DefVec(values.begin(), values.end());
    std::ranges::sort(sorted, {}, &Def::gid);
    return std::format("{}", fe::Join(sorted, ", "));
}

} // namespace

void Live::visit(Lam* fun) {
    // A closed basic block belongs to the function that jumps to it.
    if (!Lam::isa_mut_returning(fun)) return;

    auto out  = Out(std::string(arg_value(args(), "live").value_or("-")));
    auto& os  = *out.os();
    auto live = Liveness(fun);
    std::println(os, "fun {}", fun);
    for (const auto& block : live.blocks())
        std::println(os, "  {}: in {{{}}} out {{{}}} max {} flags {}", block.lam, str(block.live_in),
                     str(block.live_out), block.max_live[Liveness::Gpr], block.max_live[Liveness::Flags]);
}

} // namespace mim::plug::x86::phase
