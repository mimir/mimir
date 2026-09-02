#include "mim/plug/tensor/tensor.h"

#include <mim/plugin.h>

#include "mim/plug/tensor/phase/fuse.h"
#include "mim/plug/tensor/phase/lower.h"
#include "mim/plug/tensor/phase/lower_get_set.h"
#include "mim/plug/tensor/phase/lower_map_reduce.h"
#include "mim/plug/tensor/phase/lower_to_mem.h"
#include "mim/plug/tensor/phase/reassoc.h"

using namespace mim;
using namespace mim::plug;

namespace mim::plug::tensor {
void reg_phases(Flags2Phases& phases) {
    Phase::hook<reassoc, phase::Reassoc>(phases);
    Phase::hook<lower_tensor, phase::Lower>(phases);
    Phase::hook<lower_map_reduce, phase::LowerMapReduce>(phases);
    Phase::hook<lower_get_set, phase::LowerGetSet>(phases);
    Phase::hook<fuse_tensor, phase::Fuse>(phases);
    Phase::hook<lower_to_mem, phase::LowerToMem>(phases);
}
} // namespace mim::plug::tensor

// clang-format off
static constexpr PluginArg known_args[] = {
    {"reassoc-max=<n>", "Longest matrix chain whose bracketings `%%tensor.reassoc` enumerates (default `4`). Where no single bracketing is cheapest for *every* extent, the survivors are dispatched over at run time; a longer chain is only reassociated where one bracketing provably wins for every extent. The number of bracketings is `Catalan(n − 1)`, so raising this gets expensive fast, while anything below `3` switches the dispatch off."},
};
// clang-format on

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"tensor", MIM_VERSION, tensor::register_normalizers, tensor::reg_phases, known_args, std::size(known_args)};
}
