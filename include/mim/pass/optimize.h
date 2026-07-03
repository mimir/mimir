#pragma once

namespace mim {

class World;

/// Runs `_compile` or `_default_compile`, if available (in this order).
void optimize(World&);

} // namespace mim
