#include "mim/plug/regex/dfa2matcher.h"

#include <algorithm>
#include <sstream>

#include <absl/container/flat_hash_map.h>
#include <automaton/dfa.h>
#include <automaton/range_helper.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

template<>
struct std::formatter<automaton::DFA> : fe::ostream_formatter {};

using namespace mim;
using namespace automaton;

using Range  = automaton::Range;
using Ranges = Vector<Range>;

// nomenclature:
// c is the character from the string we want to match

// see lit/regex/match_manual.mim for a hand written state machine impl that this is based on

// the main idea is:
// for every state in the DFA, we create a lam that checks if the char c at pos is the end of the string \0
// if not, jump to a checker lam, that consists of a few bit-wise or-ed in-range checks to verify if we can transition
// to a certain other state with c if any of the checks is true, we jump to the corresponding state lam with the updated
// position if the check fails, we jump to the next checker lam or the exit lam if we checked all possible transitions
// Note, since the DFA stores the transitions as chars, not ranges, we have to merge the transitions to ranges first
// (cf. transitions_to_ranges)

namespace {

namespace core = plug::core;
namespace mem  = plug::mem;

std::string state_to_name(const DFANode* state) {
    std::stringstream ss;
    ss << "state_" << state;
    return ss.str();
}

DFAMap<Ranges> transitions_to_ranges(World& w, const DFANode* state) {
    DFAMap<Ranges> state2ranges;
    state->for_transitions([&](std::uint16_t transition, const DFANode* next_state) {
        if (!state2ranges.contains(next_state))
            state2ranges.try_emplace(next_state, Ranges{
                                                     {transition, transition}
            });
        else
            state2ranges[next_state].emplace_back(transition, transition);
    });
    Range any_range{0, 255};
    for (auto& [state, ranges] : state2ranges) {
        if (std::ranges::contains(ranges, any_range)) {
            ranges = {any_range};
            continue;
        }

        std::sort(ranges.begin(), ranges.end(), RangeCompare{});
        ranges = merge_ranges(ranges, [&w](std::string_view msg) { w.DLOG("{}", msg); });
    }
    return state2ranges;
}

const Def* match_range(const Def* c, nat_t lo, nat_t hi) {
    World& w = c->world();
    if (lo == 0 && hi == 255) return w.lit_tt();

    // let in_range     = %core.bit2.and_ 0 (%core.icmp.uge (char, lower),  %core.icmp.ule (char, upper));
    auto below_hi = w.call(core::icmp::ule, w.tuple({c, w.lit_i8(hi)}));
    auto above_lo = w.call(core::icmp::uge, w.tuple({c, w.lit_i8(lo)}));
    return w.call(core::bit2::and_, w.lit_nat(2), w.tuple({below_hi, above_lo}));
}

DFAMap<const Def*> create_check_match_transitions_from(const Def* c, const DFANode* state) {
    World& w = c->world();
    DFAMap<const Def*> state2check;

    auto state2ranges = transitions_to_ranges(w, state);

    for (auto& [state, ranges] : state2ranges) {
        for (auto& [lo, hi] : ranges)
            if (!state2check.contains(state))
                state2check.try_emplace(state, match_range(c, lo, hi));
            else
                state2check[state]
                    = w.call(core::bit2::or_, w.lit_nat(2), w.tuple({state2check[state], match_range(c, lo, hi)}));
    }
    return state2check;
}

} // namespace

extern "C" const Def* dfa2matcher(World& w, const DFA& dfa, const Def* n) {
    w.DLOG("dfa to match: {}", dfa);

    auto states = dfa.get_reachable_states();
    DFAMap<Lam*> state2matcher;

    // ((mem: %mem.M 0, string: Str n, pos: Idx n), Cn [%mem.M 0, Bool, Idx n])
    auto matcher = w.mut_fun({w.call<mem::M>(0), w.call<mem::Ptr0>(w.arr(n, w.type_i8())), w.type_idx(n)},
                             {w.call<mem::M>(0), w.type_bool(), w.type_idx(n)});
    matcher->debug_prefix(std::string("match_regex"));
    auto [args, exit] = matcher->vars<2>();
    exit->debug_prefix(std::string("exit"));
    auto [mem, string, pos] = args->projs<3>();
    mem->debug_prefix(std::string("mem"));
    string->debug_prefix(std::string("string"));
    pos->debug_prefix(std::string("pos"));

    auto error = mem::mut_con(w.type_idx(n));
    error->debug_prefix("error");
    {
        auto [mem, pos] = error->vars<2>();
        mem->debug_prefix(std::string("mem"));
        pos->debug_prefix(std::string("pos"));
        error->app(false, exit, {mem, w.lit_ff(), pos});
    }

    auto accept = mem::mut_con(w.type_idx(n));
    accept->debug_prefix("accept");
    {
        auto [mem, pos] = accept->vars<2>();
        mem->debug_prefix(std::string("mem"));
        pos->debug_prefix(std::string("pos"));
        accept->app(false, exit, {mem, w.lit_tt(), pos});
    }

    auto exiting = [error, accept](const DFANode* state) { return state->is_accepting() ? accept : error; };

    for (auto state : states) {
        auto lam = mem::mut_con(w.type_idx(n));
        lam->debug_prefix(state_to_name(state));
        state2matcher.emplace(state, lam);
    }

    for (auto [state, lam] : state2matcher) {
        auto [mem, i] = lam->vars<2>();

        if (state->is_erroring()) {
            lam->app(true, error, {mem, i});
            continue;
        }

        auto lea       = w.call<mem::lea>(Defs{string, i});
        auto [mem2, c] = w.call<mem::load>(Defs{mem, lea})->projs<2>();

        auto is_end  = w.call(core::icmp::e, Defs({c, w.lit_i8(0)}));
        auto not_end = mem::mut_con(w.type_idx(n));
        not_end->debug_prefix("not_end_" + state_to_name(state));

        auto new_i = w.call(core::wrap::add, core::Mode::nusw, w.tuple({i, w.call(core::conv::u, n, w.lit_i64(1))}));
        lam->app(false, w.select(is_end, exiting(state), not_end), {mem2, i});

        auto transitions = create_check_match_transitions_from(c, state);
        auto next_check  = exiting(state); // if we want to check full string only, use error instead of exiting(state)c
        for (auto [next_state, check] : transitions) {
            auto next_lam = state2matcher[next_state];
            auto checker  = mem::mut_con(w.type_idx(n));
            checker->debug_prefix("check_" + state_to_name(state) + "_to_" + state_to_name(next_state));
            auto [mem3, pos] = checker->vars<2>();
            checker->app(false, w.select(check, next_lam, next_check), {mem3, w.select(check, new_i, pos)});
            next_check = checker;
        }
        {
            auto [mem, pos] = not_end->vars<2>();
            not_end->app(true, next_check, {mem, pos});
        }
    }

    matcher->app(false, state2matcher[dfa.get_start()], {mem, pos});
    return matcher;
}
