#pragma once

#include <ostream>

namespace mim {

class World;

namespace sexpr {

void emit(World&, std::ostream&);
void emit_slotted(World&, std::ostream&);

} // namespace sexpr

} // namespace mim
