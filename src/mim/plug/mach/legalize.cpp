#include "mim/plug/mach/legalize.h"

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/mach/mach.h"

namespace mim::plug::mach {

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

nat_t trait(core::trait t, const Def* type) {
    if (auto n = Lit::isa(core::op(t, type))) return *n;
    type->blame("type `{}` has no static size or alignment", type).bail();
}

/// The byte offset of field @p i of @p sigma.
nat_t field_offset(const Def* sigma, nat_t i) {
    auto fields = sigma->projs();
    if (auto l = core::layout(Defs(fields).first(i + 1))) return l->offsets[i];
    sigma->blame("type `{}` has no static size or alignment", sigma).bail();
}

void check_addr_space(const App* app, const Def* a) {
    if (auto l = Lit::isa(a); !l || *l != 0)
        app->blame("native backend: address space `{}` of `{}` is not 0", a, app->axm()).bail();
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

/// A literal odd `n` of `conv.u n y` is a remainder, a dynamic one is ignored.
const Def* Legalize::index(const Def* i) {
    if (auto conv = Axm::isa(core::conv::u, i)) {
        if (!Idx::isa_lit(i->type())) return rewrite(conv->arg());
        if (auto n = core::isa_odd_idx(i->type()))
            return new_world().call<core::rem>(new_world().lit_nat(*n), rewrite(conv->arg()));
    }
    return rewrite(i);
}

const Def* Legalize::rewrite_imm_Lit(const Lit* lit) {
    if (lit->type()->isa<Nat>()) return new_world().lit_nat(lit->get());
    return RWPhase::rewrite_imm_Lit(lit);
}

const Def* Legalize::coerce_leaf(const Def* type, const Def* val) {
    auto& w = new_world();
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

    if (auto lea = Axm::isa<mem::lea>(app)) {
        auto [p, i]    = lea->args<2>();
        auto pointee   = mem::pointee(p);
        auto l         = Lit::isa(i);
        const Def* off = nullptr;
        if (!pointee->isa<Arr>()) {
            if (!l) i->blame("index `{}` into `{}` must be a literal", i, pointee).bail();
            off = w.lit_i64(field_offset(pointee, *l));
        } else if (auto size = trait(core::trait::size, mem::pointee(lea)); l) {
            off = w.lit_i64(*l * size);
        } else {
            off = core::zext(w, index(i));
            if (size != 1) off = w.call(core::wrap::mul, core::Mode::none, Defs{off, w.lit_i64(size)});
        }
        return w.call(core::wrap::add, core::Mode::none, Defs{rewrite(p), off});
    }

    if (auto load = Axm::isa<mem::load>(app)) {
        auto [T, a] = load->decurry()->args<2>();
        auto [m, p] = load->args<2>();
        check_addr_space(app, a);
        return w.call<mach::load>(rewrite(T), Defs{rewrite(m), rewrite(p)});
    }

    if (auto store = Axm::isa<mem::store>(app)) {
        auto [m, p, x] = store->args<3>();
        check_addr_space(app, store->decurry()->arg(2, 1));
        return w.call<mach::store>(Defs{rewrite(m), rewrite(p), rewrite(x)});
    }

    // A view of an array changes only the pointee.
    if (auto assume = Axm::isa<mem::assume>(app)) return rewrite(assume->arg(2, 0));

    if (auto mslot = Axm::isa<mem::mslot>(app)) {
        auto [msz, k]  = mslot->args<2>();
        auto [m, size] = msz->projs<2>();
        check_addr_space(app, mslot->decurry()->arg(2, 1));
        auto n = Lit::isa(size);
        if (!n) app->blame("native backend: `mem.mslot` needs a literal size").bail();
        return w.app(rewrite(k), w.call<slot>(w.lit_nat(*n), rewrite(m)));
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

    if (auto i2n = Axm::isa<core::idx2nat>(app)) return core::zext(w, rewrite(i2n->arg()));

    if (auto cast = Axm::isa<core::bitcast>(app)) {
        auto type = rewrite(cast->type());
        auto x    = val(cast->arg());
        return x->type() == type ? x : w.call<core::bitcast>(type, x);
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::mach
