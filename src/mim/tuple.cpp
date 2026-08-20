#include "mim/tuple.h"

#include <cassert>

#include "mim/tuple.h"
#include "mim/world.h"

namespace mim {

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
