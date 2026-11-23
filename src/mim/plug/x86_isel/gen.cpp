#include "mim/plug/x86_isel/gen.h"

#include <mim/plug/x86/autogen.h>
#include <mim/plug/x86/insn.h>

#include "mim/plug/x86_isel/x86_isel.h"

namespace mim::plug::x86_isel {

using native::rules::Want;

Gen::Gen(World& world, flags_t annex)
    : native::Gen(world,
                  annex,
                  "x86",
                  x86::Plugin_Id,
                  "x86_isel",
                  Plugin_Id,
                  {
                      .addr     = Annex::base<addr>(),
                      .reg_imm  = Annex::base<reg_imm>(),
                      .reg_sh   = Annex::base<reg_sh>(),
                      .scaled   = Annex::base<scaled>(),
                      .imm      = Annex::base<imm>(),
                      .lit      = Annex::base<lit>(),
                      .commutes = Annex::base<commutes>(),
                      .formats  = Annex::base<formats>(),
                      .sizes    = Annex::base<sizes>(),
                  }) {}

u32 Gen::cost(const App* insn) const { return x86::insn::cost(insn); }

std::optional<size_t> Gen::tied(const App* insn) const { return x86::insn::tied(insn); }

/// An `x86.Addr i` is a Base, an Index, or, for a dynamic `i`, either.
std::optional<Want> Gen::classify(const Def* type) const {
    if (auto addr = Axm::isa<x86::Addr>(type)) {
        auto i = Lit::isa(addr->arg());
        return !i ? Want::Addr : *i ? Want::Index : Want::Base;
    }
    return {};
}

} // namespace mim::plug::x86_isel
