#include "mim/plug/mem/phase/free_locals.h"

#include <fe/worklist.h>

namespace mim::plug::mem::phase {

bool FreeLocals::analyze() {
    if (RWPhase::analyze()) return true;

    auto shared = fe::Vector<std::pair<Lam*, const Def*>>();
    for (const auto& [lam, chain] : escape_.chains()) {
        for (size_t i = 0, n = chain.ops.size(); i != n; ++i) {
            auto alloc = chain.ops[i];
            if (!EscapeAnalysis::allocates(alloc) || escape_.is_top(alloc) || escape_.freed(alloc)) continue;
            if (escape_.confined(alloc)) {
                auto last = fe::lookup(chain.last, alloc);
                auto j    = (last ? *last : i) + 1;
                frees_[j < n ? chain.ops[j] : chain.tail].emplace_back(alloc);
            } else {
                shared.emplace_back(lam, alloc);
            }
        }
    }

    if (!shared.empty()) {
        auto homes = LamSet();
        for (auto [lam, _] : shared)
            homes.emplace(lam);
        // A home nested in another must hang below it, not below the virtual root.
        auto roots = fe::Vector<Def*>();
        for (auto lam : homes)
            if (std::ranges::none_of(homes, [lam](Lam* home) { return home->nests(lam); })) roots.emplace_back(lam);

        const auto nest = Nest(old_world(), roots);
        mentions(nest.root());
        for (auto [lam, alloc] : shared)
            place(nest[lam], nest[lam], alloc);
    }

    log().d("free_locals: frees before {} ops, on {} edges", frees_.size(), edges_.size());
    return false;
}

const DefSet& FreeLocals::mentions(const Nest::Node* node) {
    auto res = DefSet();
    if (auto lam = node->mut() ? node->mut()->isa_mut<Lam>() : nullptr)
        if (auto i = escape_.chains().find(lam); i != escape_.chains().end())
            for (auto [alloc, _] : i->second.last)
                res.emplace(alloc);
    for (auto child : node->children().nodes()) {
        const auto& sub = mentions(child);
        res.insert(sub.begin(), sub.end());
    }
    return mentions_[node] = std::move(res);
}

const DefSet& FreeLocals::reach(const Nest::Node* node) {
    if (auto i = reach_.find(node); i != reach_.end()) return i->second;

    auto res  = DefSet();
    auto todo = fe::BFSWorklist<GIDSet<const Nest::Node*>>();
    todo.push(node->sibl_deps().nodes());
    while (!todo.empty()) {
        auto s          = todo.pop();
        const auto& sub = mentions_.at(s);
        res.insert(sub.begin(), sub.end());
        todo.push(s->sibl_deps().nodes());
    }
    return reach_[node] = std::move(res);
}

void FreeLocals::place(const Nest::Node* home, const Nest::Node* node, const Def* alloc) {
    for (auto child : node->children().nodes())
        place(home, child, alloc);

    auto lam = node->mut()->isa_mut<Lam>();
    if (!lam || escape_.escaping(lam) || (node != home && !live(home, node, alloc))) return;
    auto i = escape_.chains().find(lam);
    if (i == escape_.chains().end()) return;

    const auto& chain = i->second;
    auto deads        = DefVec();
    for (auto target : chain.targets)
        if (dead(home, target, alloc)) deads.emplace_back(target);

    if (deads.size() == chain.targets.size())
        frees_[chain.tail].emplace_back(alloc);
    else
        for (auto target : deads)
            edges_[chain.tail][target].emplace_back(alloc);
}

bool FreeLocals::live(const Nest::Node* home, const Nest::Node* node, const Def* alloc) {
    if (mentions_.at(node).contains(alloc)) return true;
    for (auto x = node; x != home; x = x->inest())
        if (reach(x).contains(alloc)) return true;
    return false;
}

bool FreeLocals::dead(const Nest::Node* home, const Def* target, const Def* alloc) {
    auto scope = home->mut();
    if (!scope->nests(target)) return true;
    if (auto lam = target->isa_mut<Lam>()) return !live(home, home->nest()[lam], alloc);
    return false;
}

const Def* FreeLocals::rewrite_imm_App(const App* app) {
    auto allocs = fe::lookup(frees_, app);
    auto edges  = fe::lookup(edges_, app);
    if (!allocs && !edges) return RWPhase::rewrite_imm_App(app);

    auto& w  = new_world();
    auto mem = allocs ? mem_def(app->arg()) : nullptr;
    if (allocs && !mem) log().d("free_locals: `{}` threads no memory, leaking {} allocations", app, allocs->size());
    auto new_arg = mem ? thread(app->arg(), mem, *allocs) : rewrite(app->arg());

    if (!edges) return w.app(rewrite(app->callee()), new_arg);
    auto args = DefVec(app->num_targs(), [&](size_t i) {
        auto arg = app->targ(i);
        return Pi::isa_cn(arg->type()) ? rewrite_target(arg, *edges) : new_arg->proj(app->num_targs(), i);
    });
    return w.app(rewrite_target(app->callee(), *edges), w.tuple(args));
}

const Def* FreeLocals::thread(const Def* arg, const Def* mem, const DefVec& allocs) {
    if (arg == mem) return free(rewrite(mem), allocs);
    if (mem_def(arg) != mem) return rewrite(arg);
    return new_world().tuple(arg->projs([&](const Def* proj) { return thread(proj, mem, allocs); }));
}

const Def* FreeLocals::free(const Def* mem, const DefVec& allocs) {
    // TODO: address spaces other than 0 are barely tested; some, like gpu shared memory, cannot be freed.
    for (auto alloc : allocs)
        mem = new_world().call<mem::free>(Defs{mem, rewrite(alloc->proj(2, 1))});
    return mem;
}

const Def* FreeLocals::rewrite_target(const Def* target, const DefMap<DefVec>& edges) {
    auto& w = new_world();
    if (auto allocs = fe::lookup(edges, target)) return trampoline(rewrite(target), *allocs);
    if (auto tuple = target->isa<Tuple>())
        return w.tuple(DefVec(tuple->num_ops(), [&](size_t i) { return rewrite_target(tuple->op(i), edges); }));
    if (auto sw = Switch(target)) return w.extract(rewrite_target(sw.tuple(), edges), rewrite(sw.index()));
    return rewrite(target);
}

const Def* FreeLocals::trampoline(const Def* cont, const DefVec& allocs) {
    auto& w   = new_world();
    auto lam  = w.mut_con(Pi::isa_cn(cont->type())->dom())->set("free_then");
    auto args = DefVec(lam->vars());
    auto mem  = std::ranges::find_if(args, [](const Def* arg) { return isa_mem(arg); });
    if (mem == args.end()) {
        log().d("free_locals: `{}` takes no memory, leaking {} allocations", cont, allocs.size());
        return cont;
    }
    *mem = free(*mem, allocs);
    lam->app(false, cont, args);
    return lam;
}

} // namespace mim::plug::mem::phase
