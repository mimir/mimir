#include "mim/plug/regex/dfa2matcher.h"

#include <algorithm>

#include <automaton/dfa.h>
#include <automaton/range_helper.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/regex/regex.h"

#ifndef DOXYGEN
template<>
struct std::formatter<automaton::DFA> : fe::ostream_formatter {};
#endif

using namespace mim;
using namespace automaton;

using Range  = automaton::Range;
using Ranges = fe::Vector<Range>;

// nomenclature:
// c is the character from the string we want to match

// see lit/regex/match_manual.mim for a hand written state machine impl that this is based on

// the main idea is:
// For every state in the DFA, we create a lam that checks whether pos is the end of the string, i.e. n.
// If not, it loads c at pos and jumps to a chain of checker lams, each a few bit-wise or-ed in-range checks.
// A successful check jumps to the target state's lam with the next position, a failed one to the next checker or exit.
// Since the DFA stores the transitions as chars, we have to merge them to ranges first (cf. transitions_to_ranges).

namespace {

namespace core  = plug::core;
namespace mem   = plug::mem;
namespace regex = plug::regex;

// Name states by their stable id - never by pointer value, which would change from run to run.
std::string state_to_name(const DFANode* state) { return "state_" + std::to_string(state->id()); }

DFAMap<Ranges> transitions_to_ranges(World& w, const DFANode* state) {
    DFAMap<Ranges> state2ranges;
    state->for_transitions([&](std::uint16_t transition, const DFANode* next_state) {
        state2ranges[next_state].emplace_back(transition, transition);
    });
    for (auto& [state, ranges] : state2ranges) {
        std::sort(ranges.begin(), ranges.end(), RangeCompare{});
        ranges = merge_ranges(ranges, [&w](std::string_view msg) { w.log().d("{}", msg); });
    }
    return state2ranges;
}

const Def* match_range(const Def* c, nat_t lo, nat_t hi) {
    World& w = c->world();
    if (lo == 0 && hi == 255) return w.lit_tt();

    // let in_range     = core.bit2.and_ 0 (core.icmp.uge (char, lower),  core.icmp.ule (char, upper));
    auto below_hi = w.call(core::icmp::ule, Defs{c, w.lit_i8(hi)});
    auto above_lo = w.call(core::icmp::uge, Defs{c, w.lit_i8(lo)});
    return w.call(core::bit2::and_, w.lit_nat(2), w.tuple({below_hi, above_lo}));
}

DFAMap<const Def*> create_check_match_transitions_from(const Def* c, const DFANode* state) {
    World& w = c->world();
    DFAMap<const Def*> state2check;

    auto state2ranges = transitions_to_ranges(w, state);

    for (auto& [state, ranges] : state2ranges) {
        for (auto& [lo, hi] : ranges) {
            auto match = match_range(c, lo, hi);
            if (auto [i, ins] = state2check.try_emplace(state, match); !ins)
                i->second = w.call(core::bit2::or_, w.lit_nat(2), w.tuple({i->second, match}));
        }
    }
    return state2check;
}

} // namespace

extern "C" const Def* dfa2matcher(World& w, const DFA& dfa, const Def* n) {
    w.log().d("DFA to match: {}", dfa);

    auto states = dfa.get_reachable_states();
    DFAMap<Lam*> state2matcher;

    auto pos_t = w.call<regex::Pos>(n);
    auto end   = w.call<regex::pos>(n, n);
    auto one   = w.call<regex::pos>(n, w.lit_nat_1());
    auto matcher
        = w.mut_fun({w.call<mem::M>(0), w.call<mem::Str>(n), pos_t}, {w.call<mem::M>(0), w.type_bool(), pos_t});
    matcher->debug_prefix(std::string("match_regex"));
    auto [args, exit] = matcher->vars<2>();
    exit->debug_prefix(std::string("exit"));
    auto [mem, string, pos] = args->projs<3>();
    mem->debug_prefix(std::string("mem"));
    string->debug_prefix(std::string("string"));
    pos->debug_prefix(std::string("pos"));

    auto error = mem::mut_con(pos_t);
    error->debug_prefix("error");
    {
        auto [mem, pos] = error->vars<2>();
        mem->debug_prefix(std::string("mem"));
        pos->debug_prefix(std::string("pos"));
        error->app(false, exit, {mem, w.lit_ff(), pos});
    }

    auto accept = mem::mut_con(pos_t);
    accept->debug_prefix("accept");
    {
        auto [mem, pos] = accept->vars<2>();
        mem->debug_prefix(std::string("mem"));
        pos->debug_prefix(std::string("pos"));
        accept->app(false, exit, {mem, w.lit_tt(), pos});
    }

    auto exiting = [error, accept](const DFANode* state) { return state->is_accepting() ? accept : error; };

    for (auto state : states) {
        auto lam = mem::mut_con(pos_t);
        lam->debug_prefix(state_to_name(state));
        state2matcher.emplace(state, lam);
    }

    for (auto [state, lam] : state2matcher) {
        auto [mem, i] = lam->vars<2>();

        if (state->is_erroring()) {
            lam->app(true, error, {mem, i});
            continue;
        }

        auto is_end  = w.call(core::icmp::e, Defs{i, end});
        auto not_end = mem::mut_con(pos_t);
        not_end->debug_prefix("not_end_" + state_to_name(state));
        lam->app(false, w.select(is_end, exiting(state), not_end), {mem, i});

        auto [mem2, j] = not_end->vars<2>();
        auto lea       = w.call<mem::lea>(Defs{string, w.call(core::conv::u, n, j)});
        auto [mem3, c] = w.call<mem::load>(Defs{mem2, lea})->projs<2>();
        auto new_i     = w.call(core::wrap::add, core::Mode::nsuw, Defs{j, one});

        auto transitions = create_check_match_transitions_from(c, state);
        auto next_check  = exiting(state); // if we want to check full string only, use error instead of exiting(state)
        for (auto [next_state, check] : transitions) {
            auto next_lam = state2matcher[next_state];
            auto checker  = mem::mut_con(pos_t);
            checker->debug_prefix("check_" + state_to_name(state) + "_to_" + state_to_name(next_state));
            auto [mem, pos] = checker->vars<2>();
            checker->app(false, w.select(check, next_lam, next_check), {mem, w.select(check, new_i, pos)});
            next_check = checker;
        }
        not_end->app(true, next_check, {mem3, j});
    }

    matcher->app(false, state2matcher[dfa.get_start()], {mem, pos});
    return matcher;
}
