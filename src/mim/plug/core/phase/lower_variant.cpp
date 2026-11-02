#include "mim/plug/core/phase/lower_variant.h"

#include <algorithm>
#include <bit>

#include <mim/lam.h>

#include "mim/plug/core/core.h"

namespace mim::plug::core::phase {

namespace {

bool has_mem(const Def* type) {
    if (Axm::isa<M>(type)) return true;
    if (auto sigma = type->isa<Sigma>()) return std::ranges::any_of(sigma->ops(), has_mem);
    return false;
}

std::optional<nat_t> size(const Def* type) { return Lit::isa(op(trait::size, type)); }

/// The `Idx` size of a case that can share a Layout::Kind::Finite or Layout::Kind::Word.
std::optional<nat_t> idx_size(const Def* type) {
    if (auto s = Idx::isa_lit(type); s && *s != 0) return s;
    return {};
}

} // namespace

LowerVariant::Layout LowerVariant::layout(const Variant* variant) {
    if (auto res = fe::lookup(layouts_, variant)) return *res;

    auto& w = new_world();
    auto n  = variant->num_ops();
    auto l  = Layout();
    l.kind  = Layout::Kind::Finite;
    l.cases = rewrite(variant->ops());

    nat_t widest = 0;
    for (auto c : l.cases) {
        auto s = w.inhabitant(c) ? std::optional<nat_t>(1) : idx_size(c);
        if (!s || l.total > ~nat_t(0) - *s) {
            l.kind = Layout::Kind::Word;
            break;
        }
        if (*s != 1) widest = std::max(widest, size(c).value_or(0));
        l.offsets.emplace_back(l.total);
        l.total += *s;
    }
    if (l.kind == Layout::Kind::Finite && (widest == 0 || size(w.type_idx(l.total)) <= widest)) {
        l.type                   = w.type_idx(l.total);
        return layouts_[variant] = l;
    }

    l.kind     = Layout::Kind::Word;
    l.bits     = std::bit_width(n - 1);
    auto ptrs  = false;
    auto packs = std::ranges::all_of(l.cases, [&](const Def* c) {
        if (w.inhabitant(c)) return true;
        if (auto ptr = Axm::isa<Ptr>(c)) {
            auto align  = Lit::isa(op(trait::align, ptr->arg(2, 0)));
            return ptrs = align && *align >> l.bits != 0 && std::has_single_bit(*align);
        }
        auto s = idx_size(c);
        return s && Idx::size2bitwidth(*s) <= 64 - l.bits;
    });
    if (packs && ptrs) {
        l.type                   = w.type_i64();
        return layouts_[variant] = l;
    }

    l.kind    = Layout::Kind::Tagged;
    auto data = DefVec();
    for (auto c : l.cases)
        if (!w.inhabitant(c) && !std::ranges::contains(data, c)) data.emplace_back(c);
    l.type                   = w.sigma({w.type_idx(n), data.size() == 1 ? data.front() : w.call<Union>(w.tuple(data))});
    return layouts_[variant] = l;
}

const Def* LowerVariant::encode(const Layout& l, size_t i, const Def* value) {
    auto& w = new_world();
    auto c  = l.cases[i];
    switch (l.kind) {
        case Layout::Kind::Finite: {
            auto offset = w.lit_idx(l.total, l.offsets[i]);
            if (w.inhabitant(c)) return offset;
            return w.call(wrap::add, Mode::none, Defs{offset, w.call(conv::u, w.lit_nat(l.total), value)});
        }
        case Layout::Kind::Word: {
            auto tag = w.lit_i64(i);
            if (w.inhabitant(c)) return tag;
            auto bits = Axm::isa<Ptr>(c) ? w.call<bitcast>(w.type_i64(), value)
                                         : w.call(wrap::shl, Mode::none,
                                                  Defs{w.call(conv::u, w.lit_nat_0(), value), w.lit_i64(l.bits)});
            return w.call(bit2::or_, 0_n, Defs{bits, tag});
        }
        case Layout::Kind::Tagged: {
            auto box = l.type->op(1);
            auto tag = w.lit_idx(l.cases.size(), i);
            return w.tuple({tag, w.inhabitant(c) ? w.bot(box) : w.call<bitcast>(box, value)});
        }
    }
    std::unreachable();
}

const Def* LowerVariant::tag(const Layout& l, const Def* x) {
    auto& w = new_world();
    auto n  = l.cases.size();
    switch (l.kind) {
        case Layout::Kind::Finite: {
            if (l.total == n) return x;
            const Def* t = w.lit_idx(n, 0);
            for (size_t i = 1; i != n; ++i)
                t = w.extract(w.tuple({t, w.lit_idx(n, i)}),
                              w.call(icmp::uge, Defs{x, w.lit_idx(l.total, l.offsets[i])}));
            return t;
        }
        case Layout::Kind::Word: {
            auto mask = w.lit_i64((nat_t(1) << l.bits) - 1);
            return w.call(conv::u, w.lit_nat(n), w.call(bit2::and_, 0_n, Defs{x, mask}));
        }
        case Layout::Kind::Tagged: return x->proj(2, 0);
    }
    std::unreachable();
}

const Def* LowerVariant::operand(const Layout& l, const Def* x) {
    return l.kind == Layout::Kind::Tagged ? x->proj(2, 1) : x;
}

const Def* LowerVariant::decode(const Layout& l, size_t i, const Def* operand) {
    auto& w = new_world();
    auto c  = l.cases[i];
    if (auto x = w.inhabitant(c)) return x;
    switch (l.kind) {
        case Layout::Kind::Finite: {
            // The ll backend wraps a non-power-of-two Idx at its bit width, so subtract where both agree.
            auto wide = w.call(conv::u, w.lit_nat_0(), operand);
            auto rel  = w.call(wrap::sub, Mode::none, Defs{wide, w.lit_i64(l.offsets[i])});
            return w.call(conv::u, w.lit_nat(*idx_size(c)), rel);
        }
        case Layout::Kind::Word: {
            if (Axm::isa<Ptr>(c)) {
                auto mask = w.lit_i64(~((nat_t(1) << l.bits) - 1));
                return w.call<bitcast>(c, w.call(bit2::and_, 0_n, Defs{operand, mask}));
            }
            auto bits = w.call(shr::l, Defs{operand, w.lit_i64(l.bits)});
            return w.call(conv::u, w.lit_nat(*idx_size(c)), bits);
        }
        case Layout::Kind::Tagged: return w.call<bitcast>(c, operand);
    }
    std::unreachable();
}

bool LowerVariant::retype(const Axm* axm) {
    return mentions(axm->type(), [](const Def* def) { return def->isa_imm<Variant>(); });
}

const Def* LowerVariant::rewrite_imm_Variant(const Variant* variant) { return layout(variant).type; }

const Def* LowerVariant::rewrite_imm_Inj(const Inj* inj) {
    if (inj->type()->isa<Join>()) fe::throwf("cannot lower injection `{}` into a union; run join2variant first", inj);
    auto variant = inj->type()->isa_imm<Variant>();
    if (!variant) return RWPhase::rewrite_imm_Inj(inj);
    return encode(layout(variant), inj->index(), rewrite(inj->value()));
}

const Def* LowerVariant::rewrite_imm_Match(const Match* match) {
    auto variant = match->scrutinee()->unfold_type()->isa_imm<Variant>();
    if (!variant) return RWPhase::rewrite_imm_Match(match);

    auto& w = new_world();
    auto l  = layout(variant);
    auto x  = rewrite(match->scrutinee());
    auto t  = tag(l, x);
    auto op = operand(l, x);

    auto n    = l.cases.size();
    auto arms = DefVec(n);
    if (std::ranges::all_of(match->arms(), [](const Def* arm) { return Pi::isa_cn(arm->type()); })) {
        for (size_t i = 0; i != n; ++i) {
            auto arm = rewrite(match->arm(i));
            if (arm->type()->as<Pi>()->dom() == op->type() && l.cases[i] == op->type()) {
                arms[i] = arm;
            } else {
                auto lam = w.mut_con(op->type());
                arms[i]  = lam->app(false, arm, decode(l, i, lam->var()));
            }
        }
        return w.app(w.extract(w.tuple(arms), t), op);
    }

    // Evaluating every arm is only sound as long as none of them has an effect.
    if (has_mem(match->type())) fe::throwf("cannot lower effectful direct-style match `{}`", match);
    for (size_t i = 0; i != n; ++i)
        arms[i] = w.app(rewrite(match->arm(i)), decode(l, i, op));
    return w.extract(w.tuple(arms), t);
}

} // namespace mim::plug::core::phase
