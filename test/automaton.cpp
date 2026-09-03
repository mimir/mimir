#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <memory>

#include <automaton/dfa.h>
#include <automaton/dfamin.h>
#include <automaton/nfa.h>
#include <automaton/nfa2dfa.h>
#include <doctest/doctest.h>

#include <mim/world.h>

#include <mim/ast/ast.h>

#include <mim/plug/regex/dfa2matcher.h>
#include <mim/plug/regex/regex2nfa.h>

using namespace automaton;
using namespace mim;
namespace regex = mim::plug::regex;

TEST_CASE("NFA") {
    auto nfa   = std::make_unique<NFA>();
    auto start = nfa->add_state();
    nfa->set_start(start);
    auto second = nfa->add_state();
    start->add_transition(second, 'a');
    start->add_transition(second, 'b');
    second->add_transition(second, 'b');
    auto third = nfa->add_state();
    third->set_accepting(true);
    second->add_transition(third, 'a');
    third->add_transition(third, 'a');
    third->add_transition(second, 'b');

    CHECK(nfa->get_start() == start);
    CHECK_FALSE(start->is_accepting());
    CHECK(third->is_accepting());
    CHECK(start->get_transitions('a') == std::vector<const NFANode*>{second});
    CHECK(start->get_transitions('b') == std::vector<const NFANode*>{second});
    CHECK(second->get_transitions('a') == std::vector<const NFANode*>{third});
    CHECK(second->get_transitions('b') == std::vector<const NFANode*>{second});
    CHECK(third->get_transitions('a') == std::vector<const NFANode*>{third});
    CHECK(third->get_transitions('b') == std::vector<const NFANode*>{second});
}

// https://cyberzhg.github.io/toolbox/regex2nfa?regex=KGF8YikrYQ==
TEST_CASE("NFA for (a|b)+a") {
    constexpr auto eps = NFA::SpecialTransitons::EPSILON;

    auto nfa = std::make_unique<NFA>();
    std::vector<NFANode*> states;
    for (int i = 0; i < 14; ++i)
        states.push_back(nfa->add_state());
    nfa->set_start(states[0]);
    states[0]->add_transition(states[1], eps);
    states[1]->add_transition(states[2], 'a');
    states[2]->add_transition(states[5], eps);

    states[0]->add_transition(states[3], eps);
    states[3]->add_transition(states[4], 'a');
    states[4]->add_transition(states[5], eps);

    states[5]->add_transition(states[12], eps);

    states[5]->add_transition(states[6], eps);
    states[6]->add_transition(states[7], eps);
    states[7]->add_transition(states[8], 'a');
    states[8]->add_transition(states[11], eps);

    states[6]->add_transition(states[9], eps);
    states[9]->add_transition(states[10], 'b');
    states[10]->add_transition(states[11], eps);

    states[11]->add_transition(states[6], eps);
    states[11]->add_transition(states[12], eps);
    states[12]->add_transition(states[13], 'a');

    states[13]->set_accepting(true);

    CHECK(nfa->get_start() == states[0]);

    for (int i = 0; i < 13; ++i) {
        CAPTURE(i);
        CHECK_FALSE(states[i]->is_accepting());
    }
    CHECK(states[13]->is_accepting());

    std::vector<const NFANode*> empty;
    auto c = std::vector<const NFANode*>{
        {states[1], states[3]}
    };
    CHECK(states[0]->get_transitions(eps) == c);
    CHECK(states[0]->get_transitions('a') == empty);
    CHECK(states[0]->get_transitions('b') == empty);

    CHECK(states[1]->get_transitions(eps) == empty);
    CHECK(states[1]->get_transitions('a') == std::vector<const NFANode*>{states[2]});
    CHECK(states[1]->get_transitions('b') == empty);

    CHECK(states[2]->get_transitions(eps) == std::vector<const NFANode*>{states[5]});
    CHECK(states[2]->get_transitions('a') == empty);
    CHECK(states[2]->get_transitions('b') == empty);

    CHECK(states[3]->get_transitions(eps) == empty);
    CHECK(states[3]->get_transitions('a') == std::vector<const NFANode*>{states[4]});
    CHECK(states[3]->get_transitions('b') == empty);

    CHECK(states[4]->get_transitions(eps) == std::vector<const NFANode*>{states[5]});
    CHECK(states[4]->get_transitions('a') == empty);
    CHECK(states[4]->get_transitions('b') == empty);

    c = std::vector<const NFANode*>{
        {states[12], states[6]}
    };
    CHECK(states[5]->get_transitions(eps) == c);
    CHECK(states[5]->get_transitions('a') == empty);
    CHECK(states[5]->get_transitions('b') == empty);

    c = std::vector<const NFANode*>{
        {states[7], states[9]}
    };
    CHECK(states[6]->get_transitions(eps) == c);
    CHECK(states[6]->get_transitions('a') == empty);
    CHECK(states[6]->get_transitions('b') == empty);

    CHECK(states[7]->get_transitions(eps) == empty);
    CHECK(states[7]->get_transitions('a') == std::vector<const NFANode*>{states[8]});
    CHECK(states[7]->get_transitions('b') == empty);

    CHECK(states[8]->get_transitions(eps) == std::vector<const NFANode*>{states[11]});
    CHECK(states[8]->get_transitions('a') == empty);
    CHECK(states[8]->get_transitions('b') == empty);

    CHECK(states[9]->get_transitions(eps) == empty);
    CHECK(states[9]->get_transitions('a') == empty);
    CHECK(states[9]->get_transitions('b') == std::vector<const NFANode*>{states[10]});

    CHECK(states[10]->get_transitions(eps) == std::vector<const NFANode*>{states[11]});
    CHECK(states[10]->get_transitions('a') == empty);
    CHECK(states[10]->get_transitions('b') == empty);

    c = std::vector<const NFANode*>{
        {states[6], states[12]}
    };
    CHECK(states[11]->get_transitions(eps) == c);
    CHECK(states[11]->get_transitions('a') == empty);
    CHECK(states[11]->get_transitions('b') == empty);

    CHECK(states[12]->get_transitions(eps) == empty);
    CHECK(states[12]->get_transitions('a') == std::vector<const NFANode*>{states[13]});
    CHECK(states[12]->get_transitions('b') == empty);

    CHECK(states[13]->get_transitions(eps) == empty);
    CHECK(states[13]->get_transitions('a') == empty);
    CHECK(states[13]->get_transitions('b') == empty);
}

TEST_CASE("regex2nfa") {
    Driver driver;
    World& w = driver.world();
    ast::load_plugin(w, "regex");

    auto lit = [&w](char c) { return w.call<regex::lit>(w.lit_i8(c)); };
    auto nfa = [&driver](const Def* pattern) {
        pattern->dump(10);
        return regex::regex2nfa(driver.GET_FUN_PTR("regex", regex2nfa), pattern);
    };

    SUBCASE("a & b") {
        auto n = nfa(w.call<regex::conj>(Defs{lit('a'), lit('b')}));
        std::cout << *n;
    }

    SUBCASE("(a | b)+ & a") {
        auto n = nfa(w.call<regex::conj>(
            Defs{w.call(regex::quant::plus, w.call<regex::disj>(Defs{lit('a'), lit('b')})), lit('a')}));
        std::cout << *n;

        auto dfa = nfa2dfa(*n);
        std::cout << *dfa;
        std::cout << *minimize_dfa(*dfa);
    }

    SUBCASE("1 | 5 | 9") {
        auto n = nfa(w.call<regex::disj>(Defs{w.call<regex::disj>(Defs{lit('1'), lit('5')}), lit('9')}));
        std::cout << *n;

        auto dfa = nfa2dfa(*n);
        std::cout << *dfa;
        std::cout << *minimize_dfa(*dfa);
    }

    SUBCASE("!(1 | 5 | 9)") {
        auto n = nfa(
            w.call<regex::not_>(w.call<regex::disj>(Defs{w.call<regex::disj>(Defs{lit('1'), lit('5')}), lit('9')})));
        std::cout << *n;

        auto dfa = nfa2dfa(*n);
        std::cout << *dfa;
        std::cout << *minimize_dfa(*dfa);
    }

    SUBCASE("(?!\\w\\d\\s)") {
        auto n = nfa(w.call<regex::neg_lookahead>(
            w.call<regex::conj>(Defs{w.annex(regex::cls::w), w.annex(regex::cls::d), w.annex(regex::cls::s)})));
        std::cout << *n;

        auto dfa = nfa2dfa(*n);
        std::cout << *dfa;
        auto min_dfa = minimize_dfa(*dfa);
        std::cout << *min_dfa;
        driver.GET_FUN_PTR("regex", dfa2matcher)(w, *min_dfa, w.lit_nat(200))->dump(100);
    }
}

TEST_CASE("DFA") {
    auto dfa   = std::make_unique<DFA>();
    auto start = dfa->add_state();
    dfa->set_start(start);
    auto second = dfa->add_state();
    start->add_transition(second, 'a');
    start->add_transition(second, 'b');
    second->add_transition(second, 'b');
    auto third = dfa->add_state();
    third->set_accepting(true);
    second->add_transition(third, 'a');
    third->add_transition(third, 'a');
    third->add_transition(second, 'b');

    CHECK(dfa->get_start() == start);
    CHECK_FALSE(start->is_accepting());
    CHECK(third->is_accepting());
    CHECK(start->get_transition('a') == second);
    CHECK(start->get_transition('b') == second);
    CHECK(second->get_transition('a') == third);
    CHECK(second->get_transition('b') == second);
    CHECK(third->get_transition('a') == third);
    CHECK(third->get_transition('b') == second);

    std::cout << *dfa;
    std::cout << *minimize_dfa(*dfa);
}

// https://cyberzhg.github.io/toolbox/nfa2dfa?regex=KGF8YikrYQ==
TEST_CASE("DFA minimization") {
    auto dfa   = std::make_unique<DFA>();
    auto start = dfa->add_state();
    dfa->set_start(start);
    auto stateB = dfa->add_state();
    start->add_transition(stateB, 'a');
    auto stateC = dfa->add_state();
    start->add_transition(stateC, 'b');
    auto stateD = dfa->add_state();
    stateD->set_accepting(true);
    stateB->add_transition(stateD, 'a');
    stateC->add_transition(stateD, 'a');
    stateD->add_transition(stateD, 'a');
    auto stateE = dfa->add_state();
    stateB->add_transition(stateE, 'b');
    stateC->add_transition(stateE, 'b');
    stateD->add_transition(stateE, 'b');
    stateE->add_transition(stateE, 'b');
    stateE->add_transition(stateD, 'a');

    CHECK(dfa->get_start() == start);
    CHECK_FALSE(start->is_accepting());
    CHECK(stateD->is_accepting());
    CHECK(start->get_transition('a') == stateB);
    CHECK(start->get_transition('b') == stateC);
    CHECK(stateB->get_transition('a') == stateD);
    CHECK(stateB->get_transition('b') == stateE);
    CHECK(stateC->get_transition('a') == stateD);
    CHECK(stateC->get_transition('b') == stateE);
    CHECK(stateD->get_transition('a') == stateD);
    CHECK(stateD->get_transition('b') == stateE);
    CHECK(stateE->get_transition('a') == stateD);
    CHECK(stateE->get_transition('b') == stateE);

    std::cout << *dfa;
    auto min_dfa = minimize_dfa(*dfa);
    std::cout << *min_dfa;

    auto min_start = min_dfa->get_start();
    CHECK_FALSE(min_start->is_accepting());

    auto min_stateB = min_start->get_transition('a');
    CHECK(min_start->get_transition('b') == min_stateB);

    auto min_stateC = min_stateB->get_transition('a');
    CHECK(min_stateB->get_transition('b') == min_stateB);

    CHECK(min_stateC->is_accepting());
    CHECK(min_stateC->get_transition('a') == min_stateC);
    CHECK(min_stateC->get_transition('b') == min_stateB);
}
