#include "mim/plug/mem/phase/heap2stack.h"

#include <charconv>

#include <mim/plugin.h>
#include <mim/tuple.h>

namespace mim::plug::mem::phase {

bool Heap2Stack::analyze() {
    auto max = max_elems();
    if (max == 0) return false;
    if (RWPhase::analyze()) return true;

    auto nest = std::unique_ptr<const Nest>();
    for (const auto& [lam, chain] : escape_.chains()) {
        for (auto op : chain.ops) {
            auto alloc = Axm::isa<mem::alloc>(op);
            if (!alloc || escape_.is_top(op)) continue;
            auto [pointee, addr_space] = alloc->decurry()->args<2>();
            if (Lit::isa(addr_space) != 0) continue;
            if (auto n = num_elems(pointee, max); !n || *n > max) continue;
            if (!nest) nest = std::make_unique<const Nest>(old_world());
            if (in_loop((*nest)[lam])) continue;
            slots_[chain.tail].emplace_back(op);
        }
    }

    log().d("heap2stack: slots on {} chains", slots_.size());
    return false;
}

nat_t Heap2Stack::max_elems() {
    auto val = arg_value(args(), "heap2stack");
    if (!val) return Default_Max_Elems;
    auto n   = nat_t(0);
    auto end = val->data() + val->size();
    if (auto [ptr, ec] = std::from_chars(val->data(), end, n); ec == std::errc() && ptr == end) return n;
    log().w("ignoring `-X mem:heap2stack={}`: not a number", *val);
    return Default_Max_Elems;
}

std::optional<nat_t> Heap2Stack::num_elems(const Def* type, nat_t max) {
    if (type->isa_mut()) return {};

    if (auto sigma = type->isa<Sigma>()) {
        auto res = nat_t(0);
        for (auto op : sigma->ops()) {
            auto n = num_elems(op, max);
            if (!n) return {};
            res = std::min(res + *n, max + 1);
        }
        return res;
    }

    if (auto arr = type->isa<Arr>()) {
        auto res   = num_elems(arr->body(), max);
        auto shape = arr->shape();
        auto rank  = shape.rank();
        if (!res || !rank) return {};
        for (nat_t i = 0; i != *rank; ++i) {
            auto e = Shape::extent(shape[i]);
            if (!e) return {};
            res = *e != 0 && *res > (max + 1) / *e ? max + 1 : *res * *e;
        }
        return res;
    }

    return 1;
}

bool Heap2Stack::in_loop(const Nest::Node* node) {
    auto fun = node;
    while (fun->inest() && !fun->inest()->is_root())
        fun = fun->inest();
    return node->loop_depth() > fun->loop_depth();
}

const Def* Heap2Stack::rewrite_imm_App(const App* app) {
    auto allocs = fe::lookup(slots_, app);
    if (!allocs) return RWPhase::rewrite_imm_App(app);

    auto& w   = new_world();
    auto res  = static_cast<const Def*>(nullptr);
    auto curr = static_cast<Lam*>(nullptr);
    for (auto alloc : *allocs) {
        auto k    = w.mut_con(rewrite(alloc->type()))->set("slot");
        auto Ta   = rewrite(alloc->decurry()->arg());
        auto slot = w.app(w.app(w.annex<mem::slot>(), Ta), {rewrite(alloc->arg()), k});
        if (curr)
            curr->set(false, slot);
        else
            res = slot;
        map(alloc, k->var());
        curr = k;
    }
    curr->set(false, RWPhase::rewrite_imm_App(app));
    return res;
}

} // namespace mim::plug::mem::phase
