#include "mim/plug/regex/regex.h"

#include <mim/pass.h>
#include <mim/plugin.h>

#include "mim/plug/regex/pass/lower_regex.h"

using namespace mim;
using namespace mim::plug;

void reg_stages(Flags2Stages& stages) { Stage::hook<regex::lower_regex, regex::LowerRegex>(stages); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"regex", MIM_VERSION, regex::register_normalizers, reg_stages};
}
