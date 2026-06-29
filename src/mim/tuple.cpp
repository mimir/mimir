#include "mim/tuple.h"

#include <cassert>

#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

namespace {
bool should_flatten(const Def* def);

u64 flattened_size(const Seq* type) {
    if (auto a = Lit::isa(type->arity())) {
        auto n = 0;
        for (size_t i = 0; i != *a; ++i)
            if (auto inner_arr = type->proj(*a, i)->isa<Arr>())
                n += flattened_size(inner_arr);
            else
                n += 1;
        return n;
    } else {
        return 1;
    }
}

bool should_flatten(const Def* def) {
    auto type = (def->is_term() ? def->type() : def);
    if (type->isa<Sigma>()) return true;
    if (auto arr = type->isa<Arr>()) {
        if (flattened_size(arr) > type->world().flags().scalarize_threshold) return false;
        return true;
    }
    return false;
}

bool mut_val_or_type(const Def* def) {
    auto type = def->is_term() ? def->type() : def;
    return type->isa_mut();
}

const Def* unflatten(Defs defs, const Def* type, size_t& j, bool flatten_muts) {
    if (!defs.empty() && defs[0]->type() == type) return defs[j++];
    if (auto a = Lit::isa(type->arity());
        flatten_muts == mut_val_or_type(type) && a && *a != 1 && a <= type->world().flags().scalarize_threshold) {
        auto& world = type->world();
        auto ops    = DefVec(*a, [&](size_t i) { return unflatten(defs, type->proj(*a, i), j, flatten_muts); });
        return world.tuple(type, ops);
    }

    return defs[j++];
}
} // namespace

const Def* Sigma::arity() const {
    auto n = num_ops();
    if (n != 1 || isa_mut()) return world().lit_nat(n);
    return op(0)->arity();
}

const Def* Pack::arity() const {
    if (auto arr = type()->isa<Arr>()) return arr->arity();
    if (type() == world().sigma()) return world().lit_nat_0();
    return world().lit_nat_1();
}

Select::Select(const Def* def) {
    if (def) {
        if (auto extract = def->isa<Extract>(); extract && !Lit::isa(extract->index())) {
            if (auto a = Lit::isa(extract->tuple()->arity()); a && a == 2) extract_ = extract;
        }
    }
}

Branch::Branch(const Def* def)
    : Select(def->isa<App>() ? def->as<App>()->callee() : nullptr) {
    if (extract()) app_ = def->as<App>();
}

const Def* Branch::callee() const { return app()->callee(); }
const Def* Branch::arg() const { return app()->arg(); }

Dispatch::Dispatch(const Def* def) {
    if (auto app = def->isa<App>()) {
        if (auto extract = app->callee()->isa<Extract>(); extract && !Lit::isa(extract->index())) {
            if (auto a = Lit::isa(extract->tuple()->arity())) {
                app_     = app;
                extract_ = extract;
            }
        }
    }
}

const Def* Dispatch::callee() const { return app()->callee(); }
const Def* Dispatch::arg() const { return app()->arg(); }

bool is_unit(const Def* def) { return def->type() == def->world().sigma(); }

std::string tuple2str(const Def* def) {
    if (def == nullptr) return {};

    auto& w  = def->world();
    auto res = std::string();
    if (auto n = Lit::isa(def->arity())) {
        for (size_t i = 0; i != *n; ++i) {
            auto elem = def->proj(*n, i);
            if (elem->type() == w.type_i8()) {
                if (auto l = Lit::isa<char>(elem)) {
                    res.push_back(*l);
                    continue;
                }
            }
            return {};
        }
    }
    return res;
}

// TODO flatten/unflatten needs to be rewritten

size_t flatten(DefVec& ops, const Def* def, bool flatten_muts) {
    if (auto a = Lit::isa(def->arity()); a && *a != 1 && should_flatten(def) && flatten_muts == mut_val_or_type(def)) {
        auto n = 0;
        for (size_t i = 0; i != *a; ++i)
            n += flatten(ops, def->proj(*a, i), flatten_muts);
        return n;
    } else {
        ops.emplace_back(def);
        return 1;
    }
}

const Def* flatten(const Def* def) {
    if (!should_flatten(def)) return def;
    DefVec ops;
    flatten(ops, def);
    return def->is_intro() ? def->world().tuple(def->type(), ops) : def->world().sigma(ops);
}

const Def* unflatten(Defs defs, const Def* type, bool flatten_muts) {
    size_t j = 0;
    auto def = unflatten(defs, type, j, flatten_muts);
    assert(j == defs.size());
    return def;
}

const Def* unflatten(const Def* def, const Def* type) { return unflatten(def->projs(Lit::as(def->arity())), type); }

/*
 * cat
 */

DefVec cat(Defs a, Defs b) {
    auto res = DefVec();
    res.reserve(a.size() + b.size());
    res.insert(res.end(), a.begin(), a.end());
    res.insert(res.end(), b.begin(), b.end());
    return res;
}

DefVec cat(nat_t n, nat_t m, const Def* a, const Def* b) {
    auto defs = DefVec();
    defs.reserve(n + m);
    for (size_t i = 0, e = n; i != e; ++i)
        defs.emplace_back(a->proj(e, i));
    for (size_t i = 0, e = m; i != e; ++i)
        defs.emplace_back(b->proj(e, i));

    return defs;
}

const Def* cat_tuple(nat_t n, nat_t m, const Def* a, const Def* b) { return a->world().tuple(cat(n, m, a, b)); }
const Def* cat_sigma(nat_t n, nat_t m, const Def* a, const Def* b) { return a->world().sigma(cat(n, m, a, b)); }

const Def* cat_tuple(World& world, Defs a, Defs b) { return world.tuple(cat(a, b)); }
const Def* cat_sigma(World& world, Defs a, Defs b) { return world.sigma(cat(a, b)); }

const Def* tuple_of_types(const Def* t) {
    auto& world = t->world();
    if (auto sigma = t->isa<Sigma>()) return world.tuple(sigma->ops());
    if (auto arr = t->isa<Arr>()) return world.pack(arr->arity(), arr->body());
    return t;
}

} // namespace mim
