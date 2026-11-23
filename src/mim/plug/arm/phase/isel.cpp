#include "mim/plug/arm/phase/isel.h"

#include <utility>

#include <mim/plugin.h>

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

namespace mim::plug::arm::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

bool is_lowered_plugin(const Axm* axm) {
    auto p = axm->plugin();
    return p == core::Plugin_Id || p == mem::Plugin_Id;
}

/// The `arm::cc` instance that tests the predicate of @p icmp.
arm::cc cc_of(Axm::IsA<core::icmp, App> icmp) {
    switch (icmp.id()) {
        case core::icmp::e: return arm::cc::eq;
        case core::icmp::ne: return arm::cc::ne;
        case core::icmp::sl: return arm::cc::lt;
        case core::icmp::sle: return arm::cc::le;
        case core::icmp::sg: return arm::cc::gt;
        case core::icmp::sge: return arm::cc::ge;
        case core::icmp::ul: return arm::cc::cc;
        case core::icmp::ule: return arm::cc::ls;
        case core::icmp::ug: return arm::cc::hi;
        case core::icmp::uge: return arm::cc::cs;
        default: icmp->blame(MIM_ARM_BE "unsupported comparison `{}`", icmp).bail();
    }
}

} // namespace

std::pair<const Def*, arm::cc> ISel::cond(const Def* c) {
    auto& w = new_world();
    if (auto icmp = Axm::isa<core::icmp>(c)) {
        auto [a, b] = icmp->args<2>([this](const Def* d) { return rewrite(d); });
        return {w.call<arm::cmp>(w.tuple({a, b})), cc_of(icmp)};
    }
    auto v = rewrite(c);
    return {w.call<arm::cmp>(w.tuple({v, w.lit(v->type(), 0)})), arm::cc::ne};
}

const Def* ISel::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (Axm::isa<core::M>(app)) return w.call<core::M>(w.lit_nat_0());

    if (auto wrap = Axm::isa<core::wrap>(app)) {
        auto [a, b] = wrap->args<2>([this](const Def* d) { return rewrite(d); });
        auto args   = w.tuple({a, b});
        switch (wrap.id()) {
            case core::wrap::add: return w.call<arm::add>(args);
            case core::wrap::sub: return w.call<arm::sub>(args);
            case core::wrap::mul: return w.call<arm::mul>(args);
            case core::wrap::shl: return w.call<arm::shl>(args);
        }
    }

    if (auto bit2 = Axm::isa<core::bit2>(app)) {
        auto [a, b] = bit2->args<2>([this](const Def* d) { return rewrite(d); });
        auto args   = w.tuple({a, b});
        switch (bit2.id()) {
            case core::bit2::and_: return w.call<arm::and_>(args);
            case core::bit2::or_: return w.call<arm::or_>(args);
            case core::bit2::xor_: return w.call<arm::xor_>(args);
            default: break;
        }
    }

    if (auto shr = Axm::isa<core::shr>(app)) {
        auto [a, b] = shr->args<2>([this](const Def* d) { return rewrite(d); });
        auto args   = w.tuple({a, b});
        return shr.id() == core::shr::a ? w.call<arm::sar>(args) : w.call<arm::shr>(args);
    }

    if (auto div = Axm::isa<core::div>(app)) {
        auto [m, xy]  = div->args<2>([this](const Def* d) { return rewrite(d); });
        auto [a, b]   = xy->projs<2>();
        auto sign     = div.id() == core::div::sdiv || div.id() == core::div::srem;
        auto quotient = sign ? w.call<arm::sdiv>(w.tuple({a, b})) : w.call<arm::udiv>(w.tuple({a, b}));
        if (div.id() == core::div::udiv || div.id() == core::div::sdiv) return w.tuple({m, quotient});
        auto product   = w.call<arm::mul>(w.tuple({quotient, b}));
        auto remainder = w.call<arm::sub>(w.tuple({a, product}));
        return w.tuple({m, remainder});
    }

    if (auto rem = Axm::isa<core::rem>(app)) {
        auto [d, x] = App::uncurry_args<2>(rem->as<App>());
        auto ld     = Lit::isa(d);
        if (!ld) rem->blame(MIM_ARM_BE "`core.rem` needs a literal divisor").bail();
        auto s  = *Idx::isa_lit(rem->type());
        auto dr = w.lit_idx(s, *ld);
        auto xr = rewrite(x);
        auto q  = w.call<arm::udiv>(w.tuple(Defs{xr, dr}));
        auto p  = w.call<arm::mul>(w.tuple(Defs{q, dr}));
        return w.call<arm::sub>(w.tuple(Defs{xr, p}));
    }

    if (auto cast = Axm::isa<core::bitcast>(app)) {
        auto x  = rewrite(cast->arg());
        auto ds = Idx::isa_lit(app->type());
        auto ss = Idx::isa_lit(x->type());
        if (!ds || !ss) return x;
        auto db = Idx::size2bitwidth(*ds);
        auto sb = Idx::size2bitwidth(*ss);
        if (db < sb) return w.call<arm::trunc>(w.lit_nat(*ds), x);
        if (db == sb) return x;
        return w.call<arm::zext>(w.lit_nat(*ds), x);
    }

    if (auto icmp = Axm::isa<core::icmp>(app)) {
        auto [flags, cc] = cond(icmp);
        return w.call(cc, flags);
    }

    if (auto conv = Axm::isa<core::conv>(app)) {
        auto x  = rewrite(conv->arg());
        auto ds = *Idx::isa_lit(app->type());
        auto ss = *Idx::isa_lit(app->arg()->type());
        if (Idx::size2bitwidth(ds) < Idx::size2bitwidth(ss)) return w.call<arm::trunc>(w.lit_nat(ds), x);
        return conv.id() == core::conv::s ? w.call<arm::sext>(w.lit_nat(ds), x) : w.call<arm::zext>(w.lit_nat(ds), x);
    }

    if (auto ld = Axm::isa<mem::load>(app)) {
        auto [m, p] = ld->args<2>([this](const Def* d) { return rewrite(d); });
        auto s      = *Idx::isa_lit(app->proj(2, 1)->type());
        return w.call<arm::load>(w.lit_nat(s), w.tuple({m, strip_ptr_cast(p)}));
    }

    if (auto st = Axm::isa<mem::store>(app)) {
        auto [m, p, v] = st->args<3>([this](const Def* d) { return rewrite(d); });
        auto s         = *Idx::isa_lit(v->type());
        return w.call<arm::store>(w.lit_nat(s), w.tuple({m, strip_ptr_cast(p), v}));
    }

    if (auto ptr = strip_ptr_cast(app); ptr != app) return rewrite(ptr);

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !Axm::isa<mem::sched>(app) && !app->type()->isa<Pi>()
                               && !app->type()->isa<Type>() && !app->type()->isa<Nat>())
        app->blame(MIM_ARM_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

const Def* ISel::rewrite_imm_Extract(const Extract* ex) {
    auto tuple = ex->tuple();
    if (!Lit::isa(ex->index()) && tuple->num_projs() == 2) {
        auto [x, y] = tuple->projs<2>();
        if (x->type() == y->type() && !x->type()->isa<Pi>() && !is_mem(x)) {
            auto& w          = new_world();
            auto [flags, cc] = cond(ex->index());
            auto [rx, ry]    = tuple->projs<2>([this](const Def* d) { return rewrite(d); });
            return w.call(cc_to_csel(cc), Defs{flags, rx, ry});
        }
    }
    return RWPhase::rewrite_imm_Extract(ex);
}

} // namespace mim::plug::arm::phase
