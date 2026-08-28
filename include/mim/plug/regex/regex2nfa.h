#pragma once

#include <automaton/nfa.h>

#include "mim/plug/regex/regex.h"

/// You can fe::dl::get this function.
/// @returns a raw pointer to automanton::NFA; use mim::regex::regex2nfa to pack it into a `std::unique_ptr`.
extern "C" MIM_EXPORT automaton::NFA* regex2nfa(const mim::Def* regex);

namespace mim::plug::regex {

std::unique_ptr<automaton::NFA> regex2nfa(const Def* regex);

inline std::unique_ptr<automaton::NFA> regex2nfa(decltype(::regex2nfa)* fptr, const Def* regex) {
    return std::unique_ptr<automaton::NFA>(fptr(regex), std::default_delete<automaton::NFA>());
}

} // namespace mim::plug::regex
