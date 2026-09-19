#include "mim/plug/regex/regex.h"

#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/regex/dfa2matcher.h"
#include "mim/plug/regex/phase/lower_regex.h"
#include "mim/plug/regex/regex2nfa.h"

using namespace mim;
using namespace mim::plug;

static void reg_phases(Flags2Phases& phases) { Phase::hook<regex::lower_regex, regex::LowerRegex>(phases); }

static const PluginSym known_syms[] = {
    MIM_PLUGIN_SYM(regex2nfa),
    MIM_PLUGIN_SYM(dfa2matcher),
};

MIM_PLUGIN_ENTRY(regex) {
    return {"regex", MIM_VERSION, regex::register_normalizers, reg_phases, {}, {}, {},
            {},      known_syms,  std::size(known_syms)};
}
