#include "mim/def.h"
#include "mim/lattice.h"
#include "mim/tuple.h"

#include "mim/plug/math/math.h"
#include "mim/plug/mem/mem.h"     // IWYU pragma: keep
#include "mim/plug/sflow/sflow.h" // IWYU pragma: keep
#include "mim/plug/spirv/be/emit.h"

namespace mim::plug::spirv {

const Def* Emitter::strip(const Def* def) {
    auto stripped = strip_rec(def);
    return stripped ? stripped : (def->type()->isa<Type>() ? (const Def*)def->world().sigma() : def->world().tuple());
}

/// Used by [strip]
const Def* Emitter::strip_rec(const Def* def) {
    auto& world = def->world();

    if (auto sigma = def->isa<Sigma>()) {
        DefVec fields{};
        for (auto field : sigma->ops())
            if (auto stripped = strip_rec(field)) fields.push_back(stripped);

        return world.sigma(fields);
    }

    if (auto sigma = def->isa<Tuple>()) {
        DefVec fields{};
        for (auto field : sigma->ops())
            if (auto stripped = strip_rec(field)) fields.push_back(stripped);

        return world.tuple(fields);
    }

    if (auto arr = def->isa<Arr>()) {
        if (auto body = strip_rec(arr->body()))
            return world.arr(arr->arity(), body);
        else
            return nullptr;
    }

    if (auto pack = def->isa<Pack>()) {
        if (auto body = strip_rec(pack->body()))
            return world.pack(pack->arity(), body);
        else
            return nullptr;
    }

    if (auto pi = def->isa<Pi>()) {
        // Pi stays Pi. CPS pi: drop return cont, lift its dom to codom.
        // Direct-style pi (no ret_pi): strip dom/codom in place.
        if (auto ret_pi = pi->ret_pi()) {
            DefVec fields{};
            for (auto field : pi->dom()->as<Sigma>()->projs().view().rsubspan(1))
                if (auto stripped = strip_rec(field)) fields.push_back(stripped);

            auto dom   = world.sigma(fields);
            auto codom = strip(ret_pi->dom());
            return world.pi(dom, codom, pi->is_implicit());
        }

        auto dom   = strip(pi->dom());
        auto codom = strip(pi->codom());
        return world.pi(dom, codom, pi->is_implicit());
    }

    if (auto extract = def->isa<Extract>()) {
        if (Axm::isa<mem::M>(def->type())) {
            emit_term(extract->tuple());
            return nullptr;
        }
        if (auto sigma = extract->tuple()->type()->isa<Sigma>()) {
            size_t count = 0;
            auto index   = Lit::as(extract->index());
            for (auto stripped : sigma->projs([this](const Def* def) { return strip_rec(def); }))
                if (!stripped) {
                    if (count < index) index--;
                } else {
                    count++;
                }

            if (count > 1)
                return world.extract(extract->tuple(), index);
            else
                return strip_rec(extract->tuple());
        }
    }

    if (Axm::isa<mem::M>(def)) return nullptr;
    if (Axm::isa<spirv::entry>(def)) return nullptr;
    if (Axm::isa<sflow::Token>(def)) return nullptr;
    if (Axm::isa<sflow::If>(def)) return nullptr;
    if (Axm::isa<sflow::Switch>(def)) return nullptr;
    if (Axm::isa<sflow::Loop>(def)) return nullptr;

    return def;
}

// Helper function to check if a type is a scalar type suitable for vectors
bool is_scalar_type(const Def* type) { return type->isa<Nat>() || Idx::isa(type) || math::isa_f(type); }

// Helper function to check if a value is constant
// copied from mim::core::ll
// TODO: this should maybe be in utils::be or something
bool is_const(const Def* def) {
    if (def->isa<Bot>()) return true;
    if (def->isa<Lit>()) return true;
    if (auto pack = def->isa_imm<Pack>()) return is_const(pack->arity()) && is_const(pack->body());

    if (auto tuple = def->isa<Tuple>()) {
        auto ops = tuple->ops();
        return std::ranges::all_of(ops, [](auto def) { return is_const(def); });
    }

    return false;
}

} // namespace mim::plug::spirv
