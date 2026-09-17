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

bool is_dim(const Def* shape) {
    auto t = shape->unfold_type();
    return t && t->zonk_mut()->isa<Nat>();
}

const Def* first_extent(const Def* shape) {
    if (is_dim(shape)) return shape;
    auto& w = shape->world();
    return w.extract(shape, w.lit(w.type_idx(shape->arity()), 0));
}

const Def* fold_shape(const Def* shape) {
    auto& w   = shape->world();
    auto is_1 = [](const Def* e) {
        auto l = Lit::isa(e);
        return l && *l == 1;
    };
    if (is_dim(shape)) return is_1(shape) ? w.tuple() : shape;

    auto r = Lit::isa(shape->arity());
    if (!r) return shape;

    auto axes = DefVec();
    for (size_t i = 0; i != *r; ++i)
        if (auto e = shape->proj(*r, i); !is_1(e)) axes.emplace_back(e);
    return axes.size() == *r ? shape : w.tuple(axes);
}

const Def* fold_index(const Def* index) {
    auto& w   = index->world();
    auto is_1 = [](const Def* i) {
        auto l = Idx::isa_lit(i->unfold_type());
        return l && *l == 1;
    };
    if (Idx::isa(index->unfold_type())) return is_1(index) ? w.tuple() : index;

    auto r = Lit::isa(index->arity());
    if (!r) return index;

    auto comps = DefVec();
    for (size_t i = 0; i != *r; ++i)
        if (auto c = index->proj(*r, i); !is_1(c)) comps.emplace_back(c);
    return comps.size() == *r ? index : w.tuple(comps);
}

const Def* fold_index(const Def* shape, const Def* index) {
    auto& w    = shape->world();
    auto r     = shape->num_projs();
    auto comps = DefVec();
    for (size_t i = 0; i != r; ++i)
        if (auto l = Lit::isa<u64>(shape->proj(r, i)); !(l && *l == 1)) comps.emplace_back(index->proj(r, i));

    // Rebuilding an unchanged @p index would eta-reduce back to it - but only after World::tuple's pack
    // normalization has alpha-compared the projections, walking @p index's whole coordinate chain per pair.
    if (comps.size() == r) return index;
    return w.tuple(comps);
}

const Def* Seq::shape() const { return op(0); }

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
    if (auto arr = t->isa<Arr>()) {
        // One entry per *top-level* element, so a fused Arr contributes its sub-arrays, not its elements.
        auto sub = arr->is_fused() ? world.drop(arr, 1) : arr->body();
        if (sub) return world.pack(arr->arity(), sub);
    }
    return t;
}

} // namespace mim
