#include <mim/world.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86 {

namespace {

/// @p x of `Idx s` as a literal of @p type, sign-extended if @p sign.
const Def* fold(const Def* type, const Def* x, bool sign) {
    auto l = Lit::isa(x);
    auto s = Idx::isa_lit(x->type());
    auto d = Idx::isa_lit(type);
    if (!l || !s || !d) return nullptr;
    auto v = *l;
    if (auto bits = Idx::size2bitwidth(*s); sign && bits < 64 && (v >> (bits - 1)) & 1) v |= ~0_n << bits;
    return type->world().lit(type, *d == 0 ? v : v % *d);
}

} // namespace

const Def* normalize_movzx(const Def* type, const Def*, const Def* x) { return fold(type, x, false); }
const Def* normalize_movsx(const Def* type, const Def*, const Def* x) { return fold(type, x, true); }
const Def* normalize_trunc(const Def* type, const Def*, const Def* x) { return fold(type, x, false); }

MIM_x86_NORMALIZER_IMPL

} // namespace mim::plug::x86
