#include "inst.h"

#include "mim/driver.h"

namespace mim::dump {

namespace {

/// Recovers the arguments of an instance of a generic Nom by matching it against the generic one.
/// The generic Nom may live in Driver::library, so even closed parts are compared structurally.
class Instantiation {
public:
    /// @p chain is the curry_chain of the annex Lam whose body is the generic Nom.
    explicit Instantiation(const fe::Vector<Lam*>& chain)
        : chain_(chain) {
        for (auto lam : chain)
            if (auto var = lam->has_var()) vars_.emplace(var);
    }

    /// `std::nullopt` if a parameter is not recoverable.
    std::optional<Args> args(const Nom* inst) {
        auto generic = chain_.back()->body()->as<Nom>();
        if (inst == generic || !match(generic->op(), inst->op())) return {};

        auto res = Args();
        for (auto lam : chain_) {
            auto var      = lam->has_var();
            auto implicit = lam->type()->is_implicit();
            if (!var) return {};
            if (auto whole = fe::lookup(subst_, var)) {
                res.emplace_back(DefVec{whole}, implicit);
                continue;
            }
            auto n = Lit::isa(var->arity());
            if (!n || *n < 2) return {};
            auto comps = DefVec(*n, nullptr);
            for (auto [g, i] : subst_)
                if (auto ex = g->isa<Extract>(); ex && ex->tuple() == var) comps[*Lit::isa(ex->index())] = i;
            if (std::ranges::contains(comps, nullptr)) return {};
            res.emplace_back(std::move(comps), implicit);
        }
        return res;
    }

private:
    bool is_param(const Def* def) const {
        if (auto ex = def->isa<Extract>(); ex && ex->index()->isa<Lit>()) def = ex->tuple();
        return vars_.contains(def);
    }

    /// Appends the axes of @p shape that World::seq keeps; `false` for a dynamic rank.
    static bool axes(Shape shape, DefVec& res) {
        if (shape.is_dim()) {
            if (Shape::extent(*shape) != 1) res.emplace_back(*shape);
            return true;
        }
        auto r = shape.rank();
        if (!r) return false;
        for (nat_t k = 0; k != *r; ++k)
            if (auto a = shape[k]; !a)
                return false;
            else if (Shape::extent(a) != 1)
                res.emplace_back(a);
        return true;
    }

    /// Is @p i what World::arr normalizes `«shape; g»` to, with @p shape already in the World of @p i?
    /// Building that Arr would mix in the generic @p g, which may live in Driver::library.
    bool is_arr(Shape shape, const Def* g, const Def* i) {
        auto dims = DefVec();
        if (!axes(shape, dims)) return false;
        auto inst = fe::lookup(subst_, g);
        auto gen  = DefVec();
        if (auto zero = std::ranges::find(dims, 0, [](const Def* a) { return Shape::extent(a); }); zero != dims.end()) {
            dims.erase(zero, dims.end());
            inst = i->world().sigma();
        } else if (inst) {
            for (auto arr = inst->isa_imm<Arr>(); arr; arr = inst->isa_imm<Arr>()) {
                if (!axes(arr->shape(), dims)) return false;
                inst = arr->body();
            }
        } else {
            for (auto arr = g->isa_imm<Arr>(); arr; arr = g->isa_imm<Arr>()) {
                if (!axes(arr->shape(), gen)) return false;
                g = arr->body();
            }
        }

        auto body = [&](const Def* ib) { return inst ? inst == ib : match(g, ib); };
        if (dims.empty() && gen.empty()) return body(i);
        auto arr  = i->isa_imm<Arr>();
        auto idim = DefVec();
        if (!arr || !axes(arr->shape(), idim) || idim.size() != dims.size() + gen.size()) return false;
        for (size_t k = 0, n = dims.size(); k != n; ++k)
            if (dims[k] != idim[k]) return false;
        for (size_t k = 0, n = gen.size(); k != n; ++k)
            if (!match(gen[k], idim[dims.size() + k])) return false;
        return body(arr->body());
    }

    bool match(const Def* g, const Def* i) {
        if (g == i) return true;
        // Only a Level has no type.
        if (!g || !i) return false;
        // A substitution maps each generic subterm to exactly one instance subterm.
        if (auto [j, ins] = subst_.emplace(g, i); !ins) return j->second == i;
        if (is_param(g)) return true;
        if (auto arr = g->isa_imm<Arr>()) {
            auto shape      = fe::lookup(subst_, *arr->shape());
            auto body       = arr->body();
            auto same_shape = shape && i->isa_imm<Arr>() && *i->as<Arr>()->shape() == shape;
            if (shape && (subst_.contains(body) || body->is_closed()) && !same_shape) return is_arr(shape, body, i);
        }
        if (auto sig = g->isa_imm<Sigma>())
            if (auto arr = i->isa_imm<Arr>();
                arr && arr->shape().is_dim() && Lit::isa(*arr->shape()) == sig->num_ops()) {
                for (auto op : sig->ops())
                    if (!match(op, arr->body())) return false;
                return match(g->type(), i->type());
            }
        if (g->node() != i->node() || g->flags() != i->flags() || g->num_ops() != i->num_ops()) return false;
        for (size_t k = 0, n = g->num_ops(); k != n; ++k)
            if (!match(g->op(k), i->op(k))) return false;
        return match(g->type(), i->type());
    }

    const fe::Vector<Lam*>& chain_;
    DefSet vars_;
    DefMap<const Def*> subst_;
};

} // namespace

Generics::Generics(World& world, std::string_view self) {
    auto lib = world.driver().library();
    for (auto w : {&world, lib == &world ? nullptr : lib})
        if (w)
            for (auto [sym, def] : w->annexes().entries())
                if (auto lam = def->isa_mut<Lam>(); lam && !sym.view().starts_with(self)) {
                    auto chain = curry_chain(lam);
                    if (auto body = chain.back()->body())
                        if (auto nom = body->isa<Nom>())
                            generics_.emplace(nom->key(), std::pair{sym, std::move(chain)});
                }
}

std::optional<std::pair<Sym, Args>> Generics::isa_inst(const Nom* nom) const {
    if (auto generic = fe::lookup(generics_, nom->key()))
        if (auto args = Instantiation(generic->second).args(nom)) return std::pair{generic->first, std::move(*args)};
    return {};
}

} // namespace mim::dump
