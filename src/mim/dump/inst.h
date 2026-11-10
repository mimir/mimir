#pragma once

#include <optional>

#include "printer.h"

namespace mim::dump {

/// The generic `anx nom`s from elsewhere - the Nom%s the annex Lam%s of an `import` return.
class Generics {
public:
    /// @p self is the `<world name>.` prefix of the annexes the dump declares itself.
    Generics(World&, std::string_view self);

    /// The annex's qualified name and the arguments @p nom instantiates it with, if it is such an instance.
    std::optional<std::pair<Sym, Args>> isa_inst(const Nom* nom) const;

private:
    /// An instance of a generic `anx nom` shares the key of the Nom its annex Lam returns.
    ankerl::unordered_dense::map<flags_t, std::pair<Sym, fe::Vector<Lam*>>> generics_;
};

} // namespace mim::dump
