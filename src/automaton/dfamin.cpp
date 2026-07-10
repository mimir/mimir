#include "automaton/dfamin.h"

#include <algorithm>
#include <memory>

#include <absl/container/btree_set.h>

#include "automaton/dfa.h"

using namespace automaton;

namespace {
#if 0
void print_set(const DFASet& set) {
    std::cout << "{";
    for (auto state : set) std::cout << state->id() << ", ";
    std::cout << "}\n";
}
#endif

DFASet get_accepting_states(const DFASet& reachableStates) {
    DFASet acceptingStates;
    for (auto state : reachableStates)
        if (state->is_accepting()) acceptingStates.insert(state);
    return acceptingStates;
}

DFASet get_erroring_states(const DFASet& reachableStates) {
    DFASet erroringStates;
    for (auto state : reachableStates)
        if (state->is_erroring()) erroringStates.insert(state);
    return erroringStates;
}

absl::btree_set<std::uint16_t> get_alphabet(const DFASet& reachableStates) {
    absl::btree_set<std::uint16_t> alphabet;
    for (auto state : reachableStates)
        state->for_transitions([&](auto c, auto) { alphabet.insert(c); });
    return alphabet;
}

DFASet operator-(const DFASet& lhs, const DFASet& rhs) {
    DFASet result;
    for (auto state : lhs)
        if (!rhs.contains(state)) result.insert(state);
    return result;
}
DFASet operator*(const DFASet& lhs, const DFASet& rhs) {
    DFASet result;
    for (auto state : lhs)
        if (rhs.contains(state)) result.insert(state);
    return result;
}

std::vector<DFASet> hopcroft(const DFASet& reachableStates) {
    const auto alphabet = get_alphabet(reachableStates);

    const auto F = get_accepting_states(reachableStates);
    const auto E = get_erroring_states(reachableStates);

    assert((F * E).empty() && "F and E must be disjoint");

    std::vector<DFASet> P = {F, E, reachableStates - F - E};
    std::vector<DFASet> W = {F, E, reachableStates - F - E};

    std::vector<DFASet> newP;
    while (!W.empty()) {
#if 0
        std::cout << "P: ";
        for (const auto& S : P) print_set(S);
        std::cout << "W: ";
        for (const auto& S : W) print_set(S);
#endif
        auto A = W.back();
        W.pop_back();
        for (auto c : alphabet) {
            DFASet X{};
            for (const auto* state : reachableStates) {
                state->for_transitions([&](auto c_, auto to) {
                    if (c_ == c && A.contains(to)) X.insert(state);
                });
            }
            newP.clear();
            for (const auto& Y : P) {
                auto YnX = Y * X;
                auto Y_X = Y - X;
                if (!YnX.empty() && !Y_X.empty()) {
                    newP.push_back(YnX);
                    newP.push_back(Y_X);
                    if (auto YWit = std::find(W.begin(), W.end(), Y); YWit != W.end()) {
                        W.erase(YWit);
                        W.push_back(YnX);
                        W.push_back(Y_X);
                    } else {
                        if (YnX.size() <= Y_X.size())
                            W.push_back(YnX);
                        else
                            W.push_back(Y_X);
                    }
                } else
                    newP.push_back(Y);
            }
            std::swap(P, newP);
        }
    }

    return P;
}

} // namespace

namespace automaton {

std::unique_ptr<DFA> minimize_dfa(const DFA& dfa) {
    const auto reachableStates = dfa.get_reachable_states();

    const auto P = hopcroft(reachableStates);

    auto minDfa = std::make_unique<DFA>();
    DFAMap<DFANode*> dfaStates;
    for (auto& X : P) {
        auto state = minDfa->add_state();
        for (auto x : X) {
            if (x->is_accepting()) state->set_accepting(true);
            if (x->is_erroring()) state->set_erroring(true);
            dfaStates.emplace(x, state);
        }
    }
    minDfa->set_start(dfaStates[dfa.get_start()]);
    for (auto& X : P) {
        if (!X.empty()) {
            auto state = dfaStates[*X.begin()];
            for (auto x : X)
                x->for_transitions([&](auto c, auto to) { state->add_transition(dfaStates[to], c); });
        }
    }
    return minDfa;
}

} // namespace automaton
