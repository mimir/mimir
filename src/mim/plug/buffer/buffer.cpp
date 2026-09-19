#include "mim/plug/buffer/buffer.h"

#include <mim/plugin.h>

#include "mim/plug/buffer/phase/lower_ptr.h"

using namespace mim;
using namespace mim::plug;

namespace mim::plug::buffer {
static void reg_phases(Flags2Phases& phases) { Phase::hook<lower_ptr, LowerPtr>(phases); }
} // namespace mim::plug::buffer

MIM_PLUGIN_ENTRY(buffer) {
    plugin.register_normalizers = buffer::register_normalizers;
    plugin.register_phases      = buffer::reg_phases;
}
