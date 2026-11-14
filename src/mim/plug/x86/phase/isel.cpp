#include "mim/plug/x86/phase/isel.h"

#include <mim/plug/core/core.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/x86/x86.h"

namespace mim::plug::x86::phase {

namespace core = mim::plug::core;
namespace mem  = mim::plug::mem;

namespace {

/// Width of the register holding an `Idx size`.
nat_t reg_width(nat_t size) { return std::bit_ceil(std::max<nat_t>(Idx::size2bitwidth(size), 8)); }

bool is_reg_size(nat_t size) { return size == 2 || size == Idx::bitwidth2size(reg_width(size)); }

nat_t trait(core::trait t, const Def* type) {
    if (auto n = Lit::isa(core::op(t, type))) return *n;
    type->blame(MIM_X86_BE "type `{}` has no static size or alignment", type).bail();
}

std::optional<nat_t> cc(core::icmp id) {
    switch (id) {
        case core::icmp::e: return 4;
        case core::icmp::ne: return 5;
        case core::icmp::ul: return 2;
        case core::icmp::uge: return 3;
        case core::icmp::ule: return 6;
        case core::icmp::ug: return 7;
        case core::icmp::sl: return 12;
        case core::icmp::sge: return 13;
        case core::icmp::sle: return 14;
        case core::icmp::sg: return 15;
        default: return {};
    }
}

bool is_lowered_plugin(const Axm* axm) {
    auto p = axm->plugin();
    return p == Annex::flags2plugin(Annex::Base<core::M>) || p == Annex::flags2plugin(Annex::Base<mem::load>)
        || p == Annex::flags2plugin(Annex::Base<math::F>);
}

} // namespace

const Def* ISel::offset(const Def* pointee, const Def* index) {
    auto& w = new_world();
    if (auto arr = pointee->isa<Arr>()) {
        auto size = trait(core::trait::size, arr->body());
        if (auto i = Lit::isa(index)) return w.lit_i64(*i * size);

        auto n = Idx::isa_lit(index->type());
        if (!n) index->blame(MIM_X86_BE "index `{}` has no static size", index).bail();
        auto i = rewrite(index);
        if (*n != 0) i = w.call<movzx>(w.lit_i64(), i);
        return size == 1 ? i : w.call<imul>(Defs{i, w.lit_i64(size)});
    }

    auto i = Lit::isa(index);
    if (!i) index->blame(MIM_X86_BE "index `{}` into `{}` must be a literal", index, pointee).bail();
    nat_t res = 0;
    for (nat_t j = 0; j <= *i; ++j) {
        auto field = pointee->proj(pointee->num_projs(), j);
        auto align = trait(core::trait::align, field);
        res        = (res + align - 1) / align * align;
        if (j != *i) res += trait(core::trait::size, field);
    }
    return w.lit_i64(res);
}

const Def* ISel::rewrite_imm_App(const App* app) {
    auto& w = new_world();
    auto rw = [this](const Def* d) { return rewrite(d); };

    if (Axm::isa<core::Ptr>(app)) return w.type_i64();

    if (auto op = Axm::isa<core::wrap>(app)) {
        auto [a, b] = op->args<2>(rw);
        switch (op.id()) {
            case core::wrap::add: return w.call<add>(Defs{a, b});
            case core::wrap::sub: return w.call<sub>(Defs{a, b});
            case core::wrap::mul: return w.call<imul>(Defs{a, b});
            case core::wrap::shl: return w.call<shl>(Defs{a, b});
        }
    } else if (auto op = Axm::isa<core::bit2>(app)) {
        auto [a, b] = op->args<2>(rw);
        switch (op.id()) {
            case core::bit2::and_: return w.call<and_>(Defs{a, b});
            case core::bit2::or_: return w.call<or_>(Defs{a, b});
            case core::bit2::xor_: return w.call<xor_>(Defs{a, b});
            default: break;
        }
    } else if (auto op = Axm::isa<core::shr>(app)) {
        auto [a, b] = op->args<2>(rw);
        return op.id() == core::shr::a ? w.call<sar>(Defs{a, b}) : w.call<shr>(Defs{a, b});
    } else if (auto op = Axm::isa<core::icmp>(app)) {
        if (auto c = cc(op.id())) {
            auto [a, b] = op->args<2>(rw);
            return w.call<x86::cc>(*c, w.call<cmp>(Defs{a, b}));
        }
    } else if (auto op = Axm::isa<core::conv>(app)) {
        auto src = Idx::isa_lit(op->arg()->type());
        auto dst = Idx::isa_lit(app->type());
        if (src && dst) {
            auto x = rewrite(op->arg());
            if (*src == *dst) return x;
            auto sw = reg_width(*src);
            auto dw = reg_width(*dst);
            if (sw >= dw) return w.call<trunc>(*dst, x);
            return op.id() == core::conv::s ? w.call<movsx>(*dst, x) : w.call<movzx>(*dst, x);
        }
    } else if (auto op = Axm::isa<mem::load>(app)) {
        auto [_, T] = app->type()->projs<2>();
        if (auto s = Idx::isa_lit(T); s && is_reg_size(*s)) {
            auto [m, p] = op->args<2>(rw);
            return w.call<load>(*s, Defs{m, p});
        }
    } else if (auto op = Axm::isa<mem::store>(app)) {
        if (auto s = Idx::isa_lit(op->arg(3, 2)->type()); s && is_reg_size(*s)) {
            auto [m, p, v] = op->args<3>(rw);
            return w.call<store>(Defs{m, p, v});
        }
    } else if (auto op = Axm::isa<mem::lea>(app)) {
        auto [p, i]       = op->args<2>();
        auto [pointee, _] = Axm::as<core::Ptr>(p->type())->args<2>();
        return w.call<add>(Defs{rewrite(p), offset(pointee, i)});
    }

    if (auto axm = app->axm(); axm && is_lowered_plugin(axm) && !app->type()->isa<Pi>() && !Axm::isa<core::M>(app)
                               && !app->type()->isa<Type>() && !app->type()->isa<Nat>())
        app->blame(MIM_X86_BE "instruction selection does not support `{}`", axm).bail();
    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::x86::phase
