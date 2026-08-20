#include "mim/tuple.h"

#include <cassert>

#include "mim/world.h"

namespace mim {

namespace {
/// The App::callee of @p def - or `nullptr`, if @p def isn't an App at all.
const Def* callee_of(const Def* def) {
    auto app = def->isa<App>();
    return app ? app->callee() : nullptr;
}
} // namespace

Select::Select(const Def* def) {
    if (!def) return;
    auto extract = def->isa<Extract>();
    if (!extract || Lit::isa(extract->index())) return;
    if (auto a = Lit::isa(extract->tuple()->arity()); a && *a == 2) extract_ = extract;
}

Branch::Branch(const Def* def)
    : Select(callee_of(def)) {
    if (extract()) app_ = def->as<App>();
}

const Def* Branch::callee() const { return app()->callee(); }
const Def* Branch::arg() const { return app()->arg(); }

Dispatch::Dispatch(const Def* def) {
    auto app = def->isa<App>();
    if (!app) return;
    auto extract = app->callee()->isa<Extract>();
    if (!extract || Lit::isa(extract->index())) return;
    if (Lit::isa(extract->tuple()->arity())) {
        app_     = app;
        extract_ = extract;
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
        res.reserve(*n);
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
    res.append_range(a);
    res.append_range(b);
    return res;
}

DefVec cat(nat_t n, nat_t m, const Def* a, const Def* b) {
    auto res = DefVec();
    res.reserve(n + m);
    for (nat_t i = 0; i != n; ++i)
        res.emplace_back(a->proj(n, i));
    for (nat_t i = 0; i != m; ++i)
        res.emplace_back(b->proj(m, i));

    return res;
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
