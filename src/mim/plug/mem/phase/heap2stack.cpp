#include "mim/plug/mem/phase/heap2stack.h"

#include <mim/driver.h>
#include <mim/nest.h>
#include <mim/tuple.h>

namespace mim::plug::mem::phase {

namespace {

// Conservative: the slot's actual lifetime ends at the nearest enclosing returning Lam, not the top-level one.
bool in_loop(const Nest::Node* node) {
    auto fun = node;
    while (fun->level() > 1)
        fun = fun->inest();
    return node->loop_depth() > fun->loop_depth();
}

} // namespace

Heap2Stack::Heap2Stack(World& world, flags_t annex)
    : RWPhase(world, annex, &escape_)
    , escape_(world)
    , max_(driver().arg_nat("mem", "heap2stack").value_or(1024)) {}

bool Heap2Stack::analyze() {
    if (max_ == 0) return false;
    if (RWPhase::analyze()) return true;

    auto nest = std::optional<const Nest>();
    for (const auto& [lam, chain] : escape_.chains()) {
        auto allocs = fe::Vector<const App*>();
        for (auto op : chain.ops) {
            auto alloc = Axm::isa<mem::alloc>(op);
            if (!alloc || escape_.is_top(op)) continue;
            auto [pointee, addr_space] = alloc->decurry()->args<2>();
            if (Lit::isa(addr_space) == 0 && num_elems(pointee)) allocs.emplace_back(op);
        }
        if (allocs.empty()) continue;
        if (!lam->is_closed()) {
            if (!nest) nest.emplace(old_world());
            if (in_loop((*nest)[lam])) continue;
        }
        converted_.insert(allocs.begin(), allocs.end());
        slots_[chain.tail] = std::move(allocs);
    }

    log().d("heap2stack: slots on {} chains", slots_.size());
    return false;
}

std::optional<nat_t> Heap2Stack::num_elems(const Def* type) const {
    if (type->isa_mut()) return {};

    if (auto sigma = type->isa<Sigma>()) {
        auto res = nat_t(0);
        for (auto op : sigma->ops()) {
            auto n = num_elems(op);
            if (!n || (res += *n) > max_) return {};
        }
        return res;
    }

    if (auto arr = type->isa<Arr>()) {
        auto res   = num_elems(arr->body());
        auto shape = arr->shape();
        auto rank  = shape.rank();
        if (!res || !rank) return {};
        for (nat_t i = 0; i != *rank; ++i) {
            auto e = Shape::extent(shape[i]);
            if (!e || (*e != 0 && *res > max_ / *e)) return {};
            *res *= *e;
        }
        return res;
    }

    return 1;
}

const Def* Heap2Stack::rewrite_imm_App(const App* app) {
    if (auto alloc = EscapeAnalysis::freed_alloc(app); alloc && converted_.contains(alloc))
        return rewrite(app->arg(2, 0));

    auto allocs = fe::lookup(slots_, app);
    if (!allocs) return RWPhase::rewrite_imm_App(app);

    auto ks = fe::Vector<Lam*>();
    for (auto alloc : *allocs) {
        auto k = new_world().mut_con(rewrite(alloc->type()))->set("slot");
        map(alloc, k->var());
        ks.emplace_back(k);
    }

    auto res = RWPhase::rewrite_imm_App(app);
    for (size_t i = ks.size(); i-- != 0;) {
        auto alloc                 = (*allocs)[i];
        auto [pointee, addr_space] = alloc->decurry()->args<2>();
        ks[i]->set(false, res);
        res = op_slot(rewrite(pointee), rewrite(addr_space), rewrite(alloc->arg()), ks[i]);
    }
    return res;
}

} // namespace mim::plug::mem::phase
