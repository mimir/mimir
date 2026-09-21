#include "mim/plug/regex/regex2nfa.h"

#include <automaton/nfa.h>
#include <fe/bitset.h>

#include <mim/lam.h>
#include <mim/world.h>

#include "mim/plug/regex/regex.h"

using namespace mim;

namespace mim::plug::regex {

namespace {

/// Every byte a transition can carry.
const auto All_Chars = fe::Bitset(256, true);

/// The bytes @p regex matches.
fe::Bitset gather_chars(const Def* regex) {
    auto res = fe::Bitset();
    for (auto arg : detail::flatten_in_arg<regex::disj>(regex)) {
        if (auto not_ = Axm::isa<regex::not_>(arg)) {
            res |= All_Chars - gather_chars(not_->arg());
        } else if (Axm::isa<regex::any>(arg)) {
            res |= All_Chars;
        } else {
            auto rng_match = Axm::isa<regex::range, false>(arg);
            assert(rng_match
                   && "as per normalizer, if we're in a 'not_' argument, we must only have disjs, not_ and ranges!");

            auto lb = Lit::as<std::uint8_t>(rng_match->arg(0));
            auto ub = Lit::as<std::uint8_t>(rng_match->arg(1));
            for (std::uint16_t c = lb; c <= ub; ++c)
                res.set(c);
        }
    }
    return res;
}

struct Regex2NfaConverter {
    Regex2NfaConverter()
        : nfa_(std::make_unique<automaton::NFA>()) {}

    void add_range_transitions(automaton::NFANode* from, automaton::NFANode* to, std::uint16_t lb, std::uint16_t ub) {
        for (auto i = lb; i <= ub; ++i)
            from->add_transition(to, i);
    }

    void add_range_transitions(automaton::NFANode* from, automaton::NFANode* to, const Def* lit0, const Def* lit1) {
        auto lb = lit0->as<Lit>()->get();
        auto ub = lit1->as<Lit>()->get();
        add_range_transitions(from, to, lb, ub);
    }

    void
    convert(const Def* regex, automaton::NFANode* start, automaton::NFANode* end, automaton::NFANode* error = nullptr) {
        if (auto conj = Axm::isa<regex::conj>(regex)) {
            auto middle = nfa_->add_state();
            convert(conj->arg(0), start, middle, error);
            convert(conj->arg(1), middle, end, error);
        } else if (auto any = Axm::isa<regex::any>(regex)) {
            add_range_transitions(start, end, 0_u16, 255);
        } else if (auto range = Axm::isa<regex::range>(regex)) {
            add_range_transitions(start, end, range->arg(0), range->arg(1));
            if (error) add_range_transitions(start, error, 0, 255);
        } else if (auto not_ = Axm::isa<regex::not_>(regex)) {
            auto first = nfa_->add_state();

            start->add_transition(first, automaton::NFA::SpecialTransitons::EPSILON);

            for (auto c : All_Chars - gather_chars(not_->arg()))
                first->add_transition(end, std::uint16_t(c));
        } else if (auto neg = Axm::isa<regex::neg_lookahead>(regex)) {
            auto first = nfa_->add_state();
            auto error = nfa_->add_state();
            error->set_erroring(true);

            start->add_transition(first, automaton::NFA::SpecialTransitons::EPSILON);

            convert(neg->arg(), first, error, end);
            first->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
        } else if (auto disj = Axm::isa<regex::disj>(regex)) {
            convert(disj->arg(0), start, end, error);
            convert(disj->arg(1), start, end, error);
        } else if (auto opt = Axm::isa(quant::optional, regex)) {
            start->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
            convert(opt->arg(), start, end);
        } else if (auto star = Axm::isa(quant::star, regex)) {
            auto m1 = nfa_->add_state();
            auto m2 = nfa_->add_state();
            start->add_transition(m1, automaton::NFA::SpecialTransitons::EPSILON);
            start->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
            m2->add_transition(m1, automaton::NFA::SpecialTransitons::EPSILON);
            m2->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
            convert(star->arg(), m1, m2);
        } else if (auto plus = Axm::isa(quant::plus, regex)) {
            auto m0 = nfa_->add_state();
            auto m1 = nfa_->add_state();
            auto m2 = nfa_->add_state();
            convert(plus->arg(), start, m0, error);
            m0->add_transition(m1, automaton::NFA::SpecialTransitons::EPSILON);
            m2->add_transition(m1, automaton::NFA::SpecialTransitons::EPSILON);
            m2->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
            m0->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
            convert(plus->arg(), m1, m2);
        } else if (auto empty = Axm::isa<regex::empty>(regex)) {
            start->add_transition(end, automaton::NFA::SpecialTransitons::EPSILON);
        }
    }

    void convert(const Def* regex) {
        auto start = nfa_->add_state();
        nfa_->set_start(start);
        auto end = nfa_->add_state();
        end->set_accepting(true);
        convert(regex, start, end);
    }

    std::unique_ptr<automaton::NFA> nfa() { return std::move(nfa_); }

private:
    std::unique_ptr<automaton::NFA> nfa_;
};

} // namespace

std::unique_ptr<automaton::NFA> regex2nfa(const Def* regex) {
    Regex2NfaConverter converter;
    converter.convert(regex);
    return converter.nfa();
}

} // namespace mim::plug::regex

extern "C" automaton::NFA* regex2nfa(const Def* regex) { return mim::plug::regex::regex2nfa(regex).release(); }
