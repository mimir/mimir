#include "mim/plug/buffer/buffer.h"

#include <mim/plugin.h>

#include "mim/plug/buffer/phase/lower_ptr.h"

using namespace mim;
using namespace mim::plug;

namespace mim::plug::buffer {
void reg_phases(Flags2Phases& phases) { Phase::hook<lower_ptr, LowerPtr>(phases); }
} // namespace mim::plug::buffer

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"buffer", MIM_VERSION, buffer::register_normalizers, buffer::reg_phases, {}, {}};
}
