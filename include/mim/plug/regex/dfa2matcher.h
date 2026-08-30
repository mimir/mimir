#pragma once

#include <automaton/dfa.h>

#include "mim/plug/regex/regex.h"

/// You can fe::dl::get this function.
extern "C" MIM_EXPORT const mim::Def* dfa2matcher(mim::World&, const automaton::DFA&, const mim::Def*);
