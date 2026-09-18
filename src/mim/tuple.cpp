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

/// @p d without the axes @p drop rejects; @p d itself if it keeps all of them.
Shape filter_axes(const Def* d, nat_t r, auto drop) {
    auto kept = DefVec();
    kept.reserve(r);
    for (nat_t i = 0; i != r; ++i)
        if (auto a = d->proj(r, i); !drop(i, a)) kept.emplace_back(a);
    // Rebuilding an unchanged @p d would eta-reduce back to it - but only after World::tuple's pack
    // normalization has alpha-compared the projections, walking @p d's whole coordinate chain per pair.
    return kept.size() == r ? d : d->world().tuple(kept);
}

/// Is @p type a @p leaf - or an aggregate of them? This is what makes a shape a shape and an index an index.
bool isa_axes(const Def* type, auto leaf) {
    if (!type) return false; // Univ has no type
    if (leaf(type)) return true;
    if (auto sigma = type->isa<Sigma>()) return std::ranges::all_of(sigma->ops(), leaf);
    if (auto arr = type->isa<Arr>()) return leaf(arr->body()->zonk());
    return false;
}
} // namespace

/*
 * Shape
 */

Shape::Shape(World& w, Defs shape)
    : Shape(w.tuple(shape)) {}

bool Shape::is_dim() const {
    auto t = def_ ? def_->unfold_type() : nullptr;
    if (!t) return false;
    if (t->isa<Nat>() || Idx::isa(t)) return true;
    return t->zonk_mut()->isa<Nat>(); // only a Hole needs the zonk
}

std::optional<nat_t> Shape::rank() const { return def_ ? Lit::isa(def_->arity()) : std::nullopt; }

const Def* Shape::front() const {
    if (is_dim()) return def_;
    if (auto r = rank()) return def_->proj(*r, 0);

    auto& w = def_->world(); // a dynamic rank cannot be projected
    return w.extract(def_, w.lit(w.type_idx(def_->arity()), 0));
}

std::optional<nat_t> Shape::extent(const Def* axis) {
    if (auto size = Idx::isa(axis->unfold_type())) return Lit::isa(size);
    return Lit::isa(axis);
}

bool Shape::isa_extents(const Def* type) {
    return isa_axes(type, [](const Def* l) { return l->isa<Nat>() != nullptr; });
}

bool Shape::isa_indices(const Def* type) {
    return isa_axes(type, [](const Def* l) { return Idx::isa(l) != nullptr; });
}

Shape Shape::slice(nat_t begin, nat_t end) const {
    auto r = rank();
    if (!r) return {};
    if (begin == 0 && end == *r) return *this;
    return def_->world().tuple(DefVec(end - begin, [&](size_t i) { return def_->proj(*r, begin + i); }));
}

Shape Shape::drop(nat_t n) const {
    auto r = rank();
    return r ? slice(n, *r) : Shape();
}

Shape Shape::operator+(Shape other) const {
    auto ra = rank(), rb = other.rank();
    if (!ra || !rb) return {};
    return cat_tuple(*ra, *rb, def_, *other);
}

Shape Shape::fold() const {
    if (is_dim()) return extent(def_) == 1 ? Shape(def_->world().tuple()) : *this;
    auto r = rank();
    if (!r) return *this;
    return filter_axes(def_, *r, [](nat_t, const Def* a) { return extent(a) == 1; });
}

Shape Shape::fold(Shape shape) const {
    auto r = shape.rank();
    if (!r) return *this;
    return filter_axes(def_, *r, [&](nat_t i, const Def*) { return extent(shape[i]) == 1; });
}

const Def* Seq::elem() const { return shape().is_fused() ? world().drop(this, 1) : body(); }

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
        if (auto elem = arr->elem()) return world.pack(arr->arity(), elem);
    }
    return t;
}

} // namespace mim
