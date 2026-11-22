#include "mim/plug/x86/phase/legalize.h"

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

core::icmp unsigned_cmp(core::ncmp id) {
    switch (id) {
        case core::ncmp::e: return core::icmp::e;
        case core::ncmp::ne: return core::icmp::ne;
        case core::ncmp::l: return core::icmp::ul;
        case core::ncmp::le: return core::icmp::ule;
        case core::ncmp::g: return core::icmp::ug;
        case core::ncmp::ge: return core::icmp::uge;
        default: std::unreachable(); // `f` and `t` normalize to literals.
    }
}

} // namespace

bool Legalize::is_value(const Def* args) {
    for (auto arg : args->projs()) {
        auto res = rewrite(arg);
        if (res->type()->isa<Nat>() && !Lit::isa(res)) return false;
    }
    return true;
}

const Def* Legalize::val(const Def* def) {
    auto res = rewrite(def);
    if (auto l = Lit::isa(res); l && res->type()->isa<Nat>()) return new_world().lit_i64(*l);
    return res;
}

const Def* Legalize::rewrite_imm_Lit(const Lit* lit) {
    if (lit->type()->isa<Nat>()) return new_world().lit_nat(lit->get());
    return RWPhase::rewrite_imm_Lit(lit);
}

const Def* Legalize::coerce_leaf(const Def* type, const Def* val) {
    auto& w = new_world();
    if (Axm::isa<core::Ptr>(type) && val->type() == w.type_i64()) return w.call<core::bitcast>(type, val);
    if (auto l = Lit::isa(val); l && type == w.type_i64() && val->type()->isa<Nat>()) return w.lit_i64(*l);
    return nullptr;
}

const Def* Legalize::rewrite_imm_Extract(const Extract* ex) {
    // A select of `Nat`s is a value, so its literals become `I64`.
    if (auto tuple = ex->tuple()->isa<Tuple>(); tuple && ex->type()->isa<Nat>() && !Lit::isa(ex->index())) {
        auto ops = DefVec(tuple->ops(), [this](const Def* op) { return val(op); });
        return new_world().extract(new_world().tuple(ops), rewrite(ex->index()));
    }
    return RWPhase::rewrite_imm_Extract(ex);
}

const Def* Legalize::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (Axm::isa<core::Ptr>(app)) return w.type_i64();

    if (auto add = Axm::isa<mem::ptradd>(app)) {
        auto [p, off] = add->args<2>([this](const Def* def) { return rewrite(def); });
        return w.call(core::wrap::add, core::Mode::none, Defs{p, off});
    }

    if (auto nat = Axm::isa<core::nat>(app); nat && is_value(nat->arg())) {
        auto [a, b] = nat->args<2>([this](const Def* def) { return val(def); });
        auto div    = [&](core::div op) {
            auto m = w.bot(w.call<core::M>(w.lit_nat_0()));
            return w.call(op, Defs{m, w.tuple({a, b})})->proj(2, 1);
        };
        switch (nat.id()) {
            case core::nat::add: return w.call(core::wrap::add, core::Mode::none, Defs{a, b});
            case core::nat::mul: return w.call(core::wrap::mul, core::Mode::none, Defs{a, b});
            case core::nat::sub: {
                auto diff = w.call(core::wrap::sub, core::Mode::none, Defs{a, b});
                return w.extract(w.tuple({diff, w.lit_i64(0)}), w.call(core::icmp::ul, Defs{a, b}));
            }
            case core::nat::div: return div(core::div::udiv);
            case core::nat::rem: return div(core::div::urem);
        }
    }

    if (auto ncmp = Axm::isa<core::ncmp>(app); ncmp && is_value(ncmp->arg())) {
        auto [a, b] = ncmp->args<2>([this](const Def* def) { return val(def); });
        return w.call(unsigned_cmp(ncmp.id()), Defs{a, b});
    }

    if (auto idx = Axm::isa<core::idx>(app); idx && is_value(idx->arg()))
        return w.call(core::conv::u, rewrite(Idx::isa(idx->type())), val(idx->arg()));

    if (auto i2n = Axm::isa<core::idx2nat>(app))
        return w.call(core::conv::u, Idx::isa(w.type_i64()), rewrite(i2n->arg()));

    if (auto cast = Axm::isa<core::bitcast>(app)) {
        auto type = rewrite(cast->type());
        auto x    = val(cast->arg());
        return x->type() == type ? x : w.call<core::bitcast>(type, x);
    }

    if (auto slot = Axm::isa<mem::mslot>(app)) {
        auto [msz, k]  = slot->args<2>();
        auto [m, size] = msz->projs<2>();
        if (!Lit::isa(size)) app->blame(MIM_X86_BE "`mem.mslot` needs a literal size").bail();
        auto alloc = w.app(w.app(w.annex<x86::slot>(), rewrite(size)), rewrite(m));
        return w.app(rewrite(k), alloc);
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
