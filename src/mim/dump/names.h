#pragma once

#include <span>

#include "mim/def.h"

namespace mim::dump {

/// Picks the identifiers a dump binds; Names::fallback spells a Def no dump bound - a diagnostic's, say.
class Names {
public:
    enum class Policy : u8 {
        Unique, ///< Def::unique_name - unique by construction, gid and all.
        Plain,  ///< Def::sym where it is an identifier, made unique per file with `_N`; a re-read keeps these as syms.
    };

    Names(Driver&, Policy, std::span<const Sym> reserved);

    /// A fresh identifier for @p def - or for nothing in particular, if `nullptr`.
    Sym pick(const Def*);
    /// Def::sym while a diagnostic renders and no other gid claimed it (see PlainNames), Def::unique_name otherwise.
    static std::string fallback(const Def*);

private:
    Driver& driver_;
    Policy policy_;
    fe::SymSet taken_;
    fe::SymMap<size_t> counters_;
};

} // namespace mim::dump
