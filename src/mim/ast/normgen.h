#pragma once

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "mim/ast/ast.h"

namespace mim::ast {

/// @p sym as a C++ identifier: a keyword gets a trailing `_`.
std::string cpp_id(Sym sym);

/// Emits the C++ normalizer @p name that the `norm` @p rules form; at bootstrap, it replaces a hand-written one.
/// A @p family's normalizer is a template over its sub-tags.
/// @p plugins receives every plugin whose annexes the generated code refers to.
void emit_norm(std::ostream& os,
               AST& ast,
               std::set<std::string>& plugins,
               Sym name,
               const std::vector<const LetDecl*>& rules,
               const AnnexInfo* family);

} // namespace mim::ast
