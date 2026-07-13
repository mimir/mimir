#include "mim/plug/regex/regex.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/regex/phase/lower_regex.h"

using namespace mim;
using namespace mim::plug;

void reg_phases(Flags2Phases& phases) { Phase::hook<regex::lower_regex, regex::LowerRegex>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"regex", MIM_VERSION, regex::register_normalizers, reg_phases};
}
