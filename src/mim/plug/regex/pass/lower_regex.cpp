#include "mim/plug/regex/pass/lower_regex.h"

#include <automaton/dfa.h>
#include <automaton/dfamin.h>
#include <automaton/nfa2dfa.h>

#include <mim/def.h>

#include <mim/plug/core/core.h>
#include <mim/plug/direct/direct.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/regex/autogen.h"
#include "mim/plug/regex/dfa2matcher.h"
#include "mim/plug/regex/regex2nfa.h"

// clang-format off
template<> struct std::formatter<automaton::DFA> : fe::ostream_formatter {};
template<> struct std::formatter<automaton::NFA> : fe::ostream_formatter {};
// clang-format on

namespace mim::plug::regex {

namespace {
const Def* wrap_in_cps2ds(const Def* callee) { return direct::op_cps2ds_dep(callee); }
} // namespace

const Def* LowerRegex::rewrite(const Def* def) {
    const Def* new_app = def;

    if (auto app = def->isa<App>()) {
        auto callee = app->callee();
        if (Axm::isa<regex::conj>(callee) || Axm::isa<regex::disj>(callee) || Axm::isa<regex::not_>(callee)
            || Axm::isa<regex::neg_lookahead>(callee) || Axm::isa<regex::range>(callee) || Axm::isa<regex::any>(callee)
            || Axm::isa<quant>(callee) || Axm::isa<regex::empty>(callee)) {
            const auto n = app->arg();
            auto nfa     = regex2nfa(callee);
            DLOG("nfa: {}", *nfa);

            auto dfa = automaton::nfa2dfa(*nfa);
            DLOG("dfa: {}", *dfa);

            auto min_dfa = automaton::minimize_dfa(*dfa);
            new_app      = wrap_in_cps2ds(dfa2matcher(world(), *min_dfa, n));
        }
    }

    return new_app;
}

} // namespace mim::plug::regex
