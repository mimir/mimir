#include "mim/plug/gpu/phase/lower_map_reduce.h"

namespace mim::plug::gpu::phase {

const Def* LowerMapReduce::rewrite_imm_App(const App* app) { return Super::rewrite_imm_App(app); }

} // namespace mim::plug::gpu::phase
