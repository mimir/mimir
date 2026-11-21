#include "mim/plug/mem/phase/legalize_ptr.h"

#include "mim/plug/mem/mem.h"

namespace mim::plug::mem::phase {

namespace {

nat_t trait(core::trait t, const Def* type) {
    if (auto n = Lit::isa(core::op(t, type))) return *n;
    type->blame("type `{}` has no static size or alignment", type).bail();
}

/// The byte offset of field @p i of @p sigma.
nat_t field_offset(const Def* sigma, nat_t i) {
    nat_t off = 0;
    for (nat_t j = 0; j <= i; ++j) {
        auto field = sigma->proj(sigma->num_projs(), j);
        auto align = trait(core::trait::align, field);
        off        = fe::pad(off, align);
        if (j != i) off += trait(core::trait::size, field);
    }
    return off;
}

/// The index @p i at register size: a literal odd `n` of `conv.u n y` is a remainder, a dynamic one is ignored.
const Def* reg_index(World& w, const Def* i) {
    auto conv = Axm::isa(core::conv::u, i);
    if (!conv) return i;
    auto n = Idx::isa(i->type());
    if (!Lit::isa(n)) return conv->arg();
    if (core::isa_odd_idx(i->type())) return w.call<core::rem>(n, conv->arg());
    return i;
}

} // namespace

const Def* LegalizePtr::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    if (auto ptr = Axm::isa<core::Ptr>(app)) return w.call<core::Ptr>(Defs{w.type_i8(), rewrite(ptr->arg(2, 1))});

    if (auto lea = Axm::isa<mem::lea>(app)) {
        auto [p, i]       = lea->args<2>();
        auto [pointee, _] = Axm::as<core::Ptr>(p->type())->args<2>();
        auto [elem, __]   = Axm::as<core::Ptr>(lea->type())->args<2>();
        auto l            = Lit::isa(i);
        const Def* off    = nullptr;
        if (!pointee->isa<Arr>()) {
            if (!l) i->blame("index `{}` into `{}` must be a literal", i, pointee).bail();
            off = w.lit_i64(field_offset(pointee, *l));
        } else if (auto size = trait(core::trait::size, elem); l) {
            off = w.lit_i64(*l * size);
        } else {
            off = core::zext(w, reg_index(w, rewrite(i)));
            if (size != 1) off = w.call(core::wrap::mul, core::Mode::none, Defs{off, w.lit_i64(size)});
        }
        return w.call<mem::ptradd>(Defs{rewrite(p), off});
    }

    // A view of an array changes only the pointee.
    if (auto assume = Axm::isa<mem::assume>(app)) return rewrite(assume->arg(2, 0));

    if (auto slot = Axm::isa<mem::mslot>(app)) {
        auto [T, a]    = slot->decurry()->args<2>();
        auto [msz, k]  = slot->args<2>();
        auto [m, size] = msz->projs<2>();
        auto n         = Lit::isa(size).value_or(trait(core::trait::size, T));
        auto msz_      = w.tuple({rewrite(m), w.lit_nat(n)});
        return w.app(w.app(w.annex<mem::mslot>(), w.tuple({w.type_i8(), rewrite(a)})), w.tuple({msz_, rewrite(k)}));
    }

    return RWPhase::rewrite_imm_App(app);
}

const Def* LegalizePtr::coerce_leaf(const Def* type, const Def* val) {
    if (Axm::isa<core::Ptr>(type) && Axm::isa<core::Ptr>(val->type()))
        return new_world().call<core::bitcast>(type, val);
    return nullptr;
}

} // namespace mim::plug::mem::phase
