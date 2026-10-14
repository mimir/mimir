#include "automaton/nfa2dfa.h"

#include <map>
#include <queue>

namespace automaton {

namespace {
/// Compares NFASet%s lexicographically by NFANode::id - never by pointer value.
struct NFASetLt {
    bool operator()(const NFASet& a, const NFASet& b) const {
        return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), NFANode::Lt{});
    }
};

// calculate epsilon closure of a set of states
NFASet epsilonClosure(const NFASet& states) {
    NFASet closure;
    std::queue<const NFANode*> stateQueue;
    for (const auto& state : states)
        stateQueue.push(state);
    while (!stateQueue.empty()) {
        auto currentState = stateQueue.front();
        stateQueue.pop();
        closure.insert(currentState);
        currentState->for_transitions([&](auto c, auto to) {
            if (c == NFA::SpecialTransitons::EPSILON) {
                if (!closure.contains(to)) stateQueue.push(to);
            }
        });
    }
    return closure;
}

NFASet epsilonClosure(const NFANode* state) { return epsilonClosure(NFASet{state}); }
} // namespace

// nfa2dfa implementation
std::unique_ptr<DFA> nfa2dfa(const NFA& nfa) {
    auto dfa = std::make_unique<DFA>();
    std::map<NFASet, DFANode*, NFASetLt> dfaStates;
    std::queue<NFASet> stateQueue;
    NFASet startState = epsilonClosure(nfa.get_start());
    dfaStates.emplace(startState, dfa->add_state());
    stateQueue.push(startState);
    while (!stateQueue.empty()) {
        auto currentState = stateQueue.front();
        stateQueue.pop();
        auto currentDfaState = dfaStates[currentState];
        std::map<std::uint16_t, NFASet> nextStates;
        // calculate next states
        for (auto& nfaState : currentState) {
            nfaState->for_transitions([&](auto c, auto to) {
                if (c == NFA::SpecialTransitons::EPSILON) return;
                nextStates[c].insert(to);
            });
        }
        // add new states for unknown next states
        for (auto& [c, tos] : nextStates) {
            auto toStateClosure = epsilonClosure(tos);
            auto [i, fresh]     = dfaStates.try_emplace(toStateClosure);
            if (fresh) {
                i->second = dfa->add_state();
                stateQueue.push(std::move(toStateClosure));
            }
            currentDfaState->add_transition(i->second, c);
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
