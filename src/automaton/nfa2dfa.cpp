#include "automaton/nfa2dfa.h"

#include <map>
#include <queue>
#include <set>
#include <unordered_map>

namespace automaton {

// calculate epsilon closure of a set of states
std::set<const NFANode*> epsilonClosure(const std::set<const NFANode*>& states) {
    std::set<const NFANode*> closure;
    std::queue<const NFANode*> stateQueue;
    for (const auto& state : states) stateQueue.push(state);
    while (!stateQueue.empty()) {
        auto currentState = stateQueue.front();
        stateQueue.pop();
        closure.insert(currentState);
        currentState->for_transitions([&](auto c, auto to) {
            if (c == NFA::SpecialTransitons::EPSILON) {
                if (closure.find(to) == closure.end()) stateQueue.push(to);
            }
        });
    }
    return closure;
}

std::set<const NFANode*> epsilonClosure(const NFANode* state) {
    return epsilonClosure(std::set<const NFANode*>{state});
}

// nfa2dfa implementation
std::unique_ptr<DFA> nfa2dfa(const NFA& nfa) {
    auto dfa = std::make_unique<DFA>();
    std::map<std::set<const NFANode*>, DFANode*> dfaStates;
    std::queue<std::set<const NFANode*>> stateQueue;
    std::set<const NFANode*> startState = epsilonClosure(nfa.get_start());
    dfaStates.emplace(startState, dfa->add_state());
    stateQueue.push(startState);
    while (!stateQueue.empty()) {
        auto currentState = stateQueue.front();
        stateQueue.pop();
        auto currentDfaState = dfaStates[currentState];
        std::map<std::uint16_t, std::set<const NFANode*>> nextStates;
        // calculate next states
        for (auto& nfaState : currentState) {
            nfaState->for_transitions([&](auto c, auto to) {
                if (c == NFA::SpecialTransitons::EPSILON) return;
                if (nextStates.find(c) == nextStates.end())
                    nextStates.try_emplace(c, std::set<const NFANode*>{to});
                else
                    nextStates[c].insert(to);
            });
        }
        // add new states for unknown next states
        for (auto& [c, tos] : nextStates) {
            auto toStateClosure = epsilonClosure(tos);
            if (dfaStates.find(toStateClosure) == dfaStates.end()) {
                dfaStates.emplace(toStateClosure, dfa->add_state());
                stateQueue.push(toStateClosure);
            }
            currentDfaState->add_transition(dfaStates[toStateClosure], c);
        }
    }
    dfa->set_start(dfaStates[startState]);
    for (auto& [state, dfaState] : dfaStates) {
        for (auto& nfaState : state) {
            if (nfaState->is_accepting()) dfaState->set_accepting(true);
            if (nfaState->is_erroring()) {
                dfaState->set_accepting(false);
                dfaState->set_erroring(true);
                break;
            }
        }
    }
    return dfa;
}

} // namespace automaton
