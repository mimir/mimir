#include <mim/world.h>

#include "mim/plug/x86/imm.h"
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

const Def* normalize_addr(const Def* type, const Def*, const Def* arg) {
    auto& w           = type->world();
    auto [disp, base] = arg->projs<2>();
    auto d            = Lit::isa(disp);
    auto b            = Lit::isa(base);
    if (!d || !b || *b == 0) return nullptr;
    if (auto sum = s64(s32(*d)) + s64(*b); fits_i32(sum)) return w.call<addr>(Defs{w.lit_i32(u32(sum)), w.lit_i64(0)});
    return w.call<addr>(Defs{disp, w.call<mov>(base)});
}

const Def* normalize_lea(const Def* type, const Def*, const Def* a) {
    if (auto ad = Axm::isa<addr>(a)) {
        auto [disp, base] = ad->args<2>();
        if (Lit::isa(disp) == 0) return base;
        if (Lit::isa(base) == 0) return type->world().lit_i64(u64(s64(s32(*Lit::isa(disp)))));
    }
    return nullptr;
}

template<cmovcc id>
const Def* normalize_cmovcc(const Def* type, const Def*, const Def* arg) {
    if constexpr (id == cmovcc::ne) {
        auto [f, x, y] = arg->projs<3>();
        if (auto t = Axm::isa<test>(f)) {
            auto [a, b] = t->args<2>();
            if (auto c = Axm::isa<cc>(a); c && a == b)
                return type->world().call(static_cast<cmovcc>(Annex::Base<cmovcc> | c.sub()), Defs{c->arg(), x, y});
        }
    }
    return nullptr;
}

MIM_x86_NORMALIZER_IMPL

} // namespace mim::plug::x86
