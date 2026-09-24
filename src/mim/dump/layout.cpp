#include "layout.h"

#include <algorithm>
#include <ranges>

#include "mim/axm.h"
#include "mim/check.h"
#include "mim/rule.h"
#include "mim/schedule.h"
#include "mim/union.h"
#include "mim/world.h"

#include "names.h"

namespace mim::dump {

using Dump = Def::Dump;

Def* isa_decl(const Def* def) {
    if (auto mut = def->isa_mut()) {
        if (mut->isa<Hole>()) return nullptr;
        if (mut->is_external() || mut->isa<Lam>() || (mut->sym() && mut->sym() != '_')) return mut;
    }
    return nullptr;
}

nat_t num_binders(const Def* type) {
    if (!type) return 1;
    auto n = std::optional<nat_t>();
    if (auto seq = type->isa<Seq>()) {
        if (seq->shape().is_fused()) return 1;
        n = Lit::isa(*seq->shape());
    } else if (auto sigma = type->isa<Sigma>()) {
        if (sigma->num_ops() == 1 && !sigma->isa_mut()) return num_binders(sigma->op(0));
        n = sigma->num_ops();
    } else {
        return 1;
    }
    return n && *n < type->world().flags().scalarize_threshold ? *n : 1;
}

bool spells_inline(const Def* def) {
    if (auto mut = def->isa_mut()) return !isa_decl(mut);
    if (def->is_closed()) return true;
    if (auto app = def->isa<App>()) {
        if (app->type()->isa<Pi>() || app->type()->isa<Type>()) return true;
        if (app->callee()->isa<Axm>()) return num_binders(app->callee_type()->dom()) <= 1;
        return false;
    }
    return true;
}

const Def* solve(const Def* def) {
    while (auto hole = def ? def->isa_mut<Hole>() : nullptr) {
        if (!hole->is_set()) break;
        def = hole->op();
    }
    return def;
}

/*
 * helpers
 */

bool Layout::fun_shape(const Lam* lam) {
    auto dom = lam->dom();
    return Lam::isa_returning(lam) && dom->isa<Sigma>() && num_binders(dom) == 2 && Pi::isa_basicblock(dom->op(1));
}

const Def* Layout::binder_type(const Def* mut) {
    if (auto lam = mut->isa<Lam>()) return lam->dom();
    if (auto pi = mut->isa<Pi>()) return pi->dom();
    if (auto rule = mut->isa<Rule>()) return rule->dom();
    if (mut->isa<Sigma>()) return mut;
    return nullptr;
}

/// The type of component @p i of @p type - if it exists without asking the frozen World.
const Def* Layout::comp_type(const Def* type, size_t i) {
    if (type->isa<Prod>()) return type->op(i);
    if (auto seq = type->isa<Seq>(); seq && !seq->isa_mut() && !seq->shape().is_fused()) return seq->body();
    return nullptr;
}

bool Layout::user_named(const Def* def) { return def && def->sym() && def->sym() != '_'; }

Layout::Layout(World& world, Opts opts)
    : world_(world)
    , opts_(std::move(opts))
    , top_{nullptr, nullptr, {}, nullptr} {}

void Layout::add_axms(std::span<const std::pair<std::string, const Axm*>> axms) {
    for (auto& [name, axm] : axms) {
        auto view = std::string_view(name);
        auto dot  = view.find('.');
        auto mod  = dot == std::string_view::npos ? Sym() : world_.sym(view.substr(0, dot));
        if (axm_groups_.empty() || axm_groups_.back().first != mod)
            axm_groups_.emplace_back(mod, fe::Vector<const Axm*>());
        auto& group = axm_groups_.back().second;
        if (group.empty()) group_keys_.emplace(axm);
        group.emplace_back(axm);
        axm2group_[axm] = axm_groups_.size() - 1;
        seeds_.emplace_back(axm->type());
    }
}

void Layout::add_root(const Def* def) {
    roots_.emplace_back(def);
    seeds_.emplace_back(def);
}

void Layout::finish(Names& names) {
    if (opts_.mode == Dump::Expr) return;
    for (auto seed : seeds_)
        reach1(seed);
    classify();
    for (auto seed : seeds_)
        reach3(seed, nullptr, nullptr, false);
    // A projection's type exists only if the World handed the projection out; it is spelled in the binder's header.
    for (size_t i = 0; i != type_work_.size(); ++i) {
        auto [type, b] = type_work_[i];
        reach1(type);
        reach3(type, b->mut, b->scope, false);
    }
    headers();
    place();
    order();
    spell();
    names_ = &names;
}

/// The Def%s the dump prints @p def with; a Var is a leaf and a Lam spells out its Pi.
template<class F>
void Layout::deps(const Def* def, F&& f) const {
    if (auto lam = def->isa_mut<Lam>()) {
        f(lam->type()->dom());
        // A chain member's codomain is the next member's Pi - spelled with the Pi's own Var, which nothing prints.
        if (!Pi::isa_cn(lam->type()) && last_in_chain(lam)) f(lam->type()->codom());
        if (lam->is_set()) f(lam->filter()), f(lam->body());
        return;
    }
    if (auto hole = def->isa_mut<Hole>()) {
        if (hole->is_set()) f(hole->op());
        return;
    }
    if (def->isa<Var>()) return;
    if (auto mut = def->isa_mut(); mut && !mut->is_set()) return;
    if (auto rule = def->isa_mut<Rule>()) f(rule->dom());
    if (opts_.typed_let || def->isa<Lit>() || def->isa<Ext>() || def->isa<Inj>()) f(def->type());
    for (auto op : def->ops())
        f(op);
}

bool Layout::descends(const Def* mut) const {
    switch (opts_.mode) {
        case Dump::All: return !(mut->is_external() && opts_.skip.contains(mut->loc().src));
        case Dump::Scope: return !(isa_decl(mut) && mut->is_closed() && !is_seed(mut));
        default: return true;
    }
}

/*
 * P1: reach
 */

void Layout::reach1(const Def* def) {
    if (!def || !reached_.emplace(def).second) return;
    order1_.emplace_back(def);
    if (auto mut = def->isa_mut()) {
        if (!descends(mut) && !is_seed(mut)) return;
        descended_.emplace(mut);
    }
    deps(def, [&](const Def* op) {
        if (!op) return;
        if (op->isa_mut()) ++uses_[op];
        reach1(op);
    });
}

/*
 * P2: classify
 */

void Layout::classify() {
    auto tt = world_.lit_tt();

    for (auto def : order1_) {
        auto match = def->isa<Match>();
        if (!match || match->num_arms() == 0) continue;
        auto ok = std::ranges::all_of(match->arms(), [&](const Def* arm) {
            auto lam = arm->isa_mut<Lam>();
            return lam && lam->is_set() && !lam->is_external() && lam->filter() == tt && uses_[lam] == 1
                && !is_seed(lam);
        });
        if (!ok) continue;
        matches_.emplace(match);
        for (auto arm : match->arms())
            arms_.emplace(arm);
    }

    for (auto def : order1_)
        if (auto mut = isa_decl(def); mut && descended_.contains(mut) && !arms_.contains(mut)) decls_.emplace(mut);

    // A set, single-use inner Lam joins the chain - as long as the member before it needs no filter of its own.
    DefSet absorbed;
    for (auto def : order1_) {
        auto lam = def->isa_mut<Lam>();
        if (!lam || !descended_.contains(lam) || absorbed.contains(lam)) continue;
        auto& chain = chains_[lam];
        chain.emplace_back(lam);
        head_[lam] = lam;
        if (!decls_.contains(lam)) continue;
        for (auto curr = lam;;) {
            if (!curr->is_set() || curr->filter() != tt) break;
            auto next = curr->body()->isa_mut<Lam>();
            if (!next || next->is_external() || !next->is_set() || arms_.contains(next) || uses_[next] != 1
                || head_.contains(next))
                break;
            chain.emplace_back(next);
            head_[next] = lam;
            absorbed.emplace(next);
            curr = next;
        }
    }

    for (auto def : order1_) {
        auto mut = def->isa_mut();
        if (!mut || !decls_.contains(mut)) continue;
        auto lam  = mut->isa_mut<Lam>();
        auto head = !lam || head_[lam] == lam;
        if (opts_.mode == Dump::Local) {
            if (head) root_heads_.emplace_back(mut);
            continue;
        }
        if (!mut->is_closed()) continue;
        scopes_.emplace_back(mut);
        if (head) root_heads_.emplace_back(mut);
    }

    // A Lam's Var binds what its Pi's Var and its anonymous dependent dom Sigma's Var stand for - as long as
    // nothing else prints them: a re-read tells the Lam's binder from a shared one, so their names must differ.
    DefMap<u32> pi_lams;
    for (auto def : order1_)
        if (auto lam = def->isa_mut<Lam>(); lam && descended_.contains(lam)) ++pi_lams[lam->type()];
    for (auto def : order1_) {
        auto mut = def->isa_mut();
        if (!mut || !descended_.contains(mut)) continue;
        auto var = mut->has_var();
        if (!var) continue;
        auto lam       = mut->isa_mut<Lam>();
        const Def* dom = nullptr;
        if (lam) {
            if (auto pv = lam->type()->has_var()) {
                if (reached_.contains(lam->type()) || pi_lams[lam->type()] > 1)
                    alias_lams_[pv].emplace_back(lam), extra_vars_[lam].emplace_back(pv);
                else
                    unite(pv, var);
            }
            dom = lam->dom();
        } else if (auto pi = mut->isa_mut<Pi>()) {
            dom = pi->dom();
        } else if (auto rule = mut->isa_mut<Rule>()) {
            dom = rule->dom();
        }
        if (!dom) continue;
        auto [sigma, sv] = dom->isa_binder<Sigma>();
        if (!sv || decls_.contains(sigma)) continue;
        if (uses_[sigma] == 1)
            unite(sv, var);
        else if (lam)
            alias_lams_[sv].emplace_back(lam), extra_vars_[lam].emplace_back(sv);
    }
}

const Var* Layout::find(const Var* var) const {
    for (auto i = alias_.find(var); i != alias_.end(); i = alias_.find(var))
        var = i->second;
    return var;
}

void Layout::unite(const Var* var, const Var* rep) {
    auto a = find(var), b = find(rep);
    if (a != b) alias_[a] = b;
}

bool Layout::is_ctx_var(const Var* var, const Lam* ctx) {
    if (!ctx || var == ctx->has_var()) return false;
    if (var == ctx->type()->has_var()) return true;
    auto [sigma, sv] = ctx->dom()->isa_binder<Sigma>();
    return sv && var == sv;
}

bool Layout::last_in_chain(const Lam* lam) const {
    auto i = head_.find(lam);
    return i == head_.end() || chains_.find(i->second)->second.back() == lam;
}

/*
 * P3: reach again - numbering, placement context, binder marks
 */

void Layout::reach3(const Def* def, Def* curr, Def* scope, bool inl) {
    if (!def || !pre3_.emplace(def).second) return;
    if (auto mut = def->isa_mut()) {
        if (!descended_.contains(mut)) return;
        auto b = ensure_binder(mut);
        if (mut->is_closed()) {
            // A closed anonymous mutable is no Nest node, so nothing inside it can be placed.
            if (decls_.contains(mut))
                scope = mut, inl = false;
            else
                inl = true;
        } else if (decls_.contains(mut) && scope) {
            open_decls_[scope].emplace_back(mut);
        }
        b->scope = scope;
        if (auto lam = mut->isa_mut<Lam>(); lam && head_[lam] == lam && decls_.contains(lam)) {
            heads_.emplace_back(lam);
            if (scope) scope_heads_[scope].emplace_back(lam);
        }
        curr = mut;
    } else if (scope && !inl && !def->is_closed() && !spells_inline(def)) {
        cands_[scope].emplace_back(def);
        cand_set_.emplace(def);
        curr_[def] = curr;
    }
    deps(def, [&](const Def* op) {
        if (!op) return;
        mark(def, op);
        reach3(op, curr, scope, inl);
    });
    seq_[def] = order3_.size();
    order3_.emplace_back(def);
}

Binder* Layout::ensure_binder(Def* mut) {
    auto var        = mut->has_var();
    auto key        = var ? find(var)->binder() : mut;
    auto [i, fresh] = binders_.try_emplace(key, nullptr);
    if (fresh) {
        auto& b      = binder_store_.emplace_back();
        b.mut        = key;
        b.root.def   = var ? find(var) : nullptr;
        b.root.type  = binder_type(key);
        b.root.owner = &b;
        i->second    = &b;
    }
    if (var && !std::ranges::contains(i->second->vars, var)) i->second->vars.emplace_back(var);
    if (auto e = extra_vars_.find(mut); e != extra_vars_.end())
        for (auto v : e->second)
            if (!std::ranges::contains(i->second->vars, v)) i->second->vars.emplace_back(v);
    return i->second;
}

Slot* Layout::resolve_(const Def* def, const Lam* ctx) {
    if (auto var = def->isa<Var>()) {
        if (is_ctx_var(var, ctx)) return &ensure_binder(const_cast<Lam*>(ctx))->root;
        return &ensure_binder(find(var)->binder())->root;
    }
    if (auto ex = def->isa<Extract>())
        if (auto l = Lit::isa(ex->index()))
            if (auto p = resolve_(ex->tuple(), ctx)) {
                expand(*p);
                if (auto n = Lit::isa(Idx::isa(ex->index()->type())); n && p->comps.size() == *n && *l < *n)
                    return &p->comps[*l];
            }
    return nullptr;
}

/// The components of @p s - as far as the frozen World can hand them out; a nested dependent Sigma keeps its own.
void Layout::expand(Slot& s) {
    if (s.expanded) return;
    s.expanded = true;
    auto root  = s.owner;

    if (auto seq = root ? root->mut->isa<Seq>() : nullptr) {
        auto r = seq->shape().rank();
        if (!r || *r <= 1) return;
        for (auto k : std::views::iota(nat_t(0), *r)) {
            auto& c = s.comps.emplace_back();
            for (auto v : root->vars)
                if ((c.def = v->proj(*r, k))) break;
        }
        return;
    }

    auto type = s.type;
    if (!type) return;
    auto n = num_binders(type);
    if (n <= 1) return;
    // A named Sigma prints by name - `x: S` - except for its own fields.
    auto own = root && root->mut == type;
    if (isa_decl(type) && !own) return;
    // A dependent Sigma nobody's Var stands for spells its fields with its own binder - unless every projection
    // exists, whose types have the binder substituted already.
    auto [sigma, sv] = type->isa_binder<Sigma>();
    auto foreign     = sv && (!root || static_cast<const Def*>(find(sv)) != s.def);

    auto has_lit = world_.lit_nat(n) != nullptr; // no `Idx n` without it - and no projection either
    for (auto i : std::views::iota(nat_t(0), n)) {
        auto& c = s.comps.emplace_back();
        if (has_lit) {
            if (root) {
                for (auto v : root->vars)
                    if ((c.def = v->proj(n, i))) break;
            } else if (s.def) {
                c.def = s.def->proj(n, i);
            }
        }
        c.type = c.def ? c.def->type() : comp_type(type, i);
        if (!c.type || (foreign && !c.def)) return s.comps.clear();
    }
    if (root)
        for (auto& c : s.comps)
            if (c.def) type_work_.emplace_back(c.type, root);
}

/// @p dep occurs in @p user: as a whole, unless @p user is the projection that names one of its components.
void Layout::mark(const Def* user, const Def* dep) {
    if (auto ex = user->isa<Extract>(); ex && dep == ex->tuple() && Lit::isa(ex->index()) && resolve_(user)) return;
    for (auto def = dep;;) {
        if (auto slot = resolve_(def)) {
            auto base = def;
            while (auto ex = base->isa<Extract>())
                base = ex->tuple();
            auto var                           = base->as<Var>();
            auto field                         = var->binder()->isa<Sigma>() != nullptr;
            (field ? slot->field : slot->used) = true;
            if (auto i = alias_lams_.find(var); i != alias_lams_.end())
                for (auto lam : i->second)
                    if (auto s = resolve_(def, lam))
                        (field ? s->field : s->used) = true;
                    else if (!field)
                        ensure_binder(lam)->root.used = true; // spelled `x#i` then
            return;
        }
        auto ex = def->isa<Extract>();
        if (!ex || !Lit::isa(ex->index())) return;
        def = ex->tuple(); // prints as `t#i`, so `t` is used whole
    }
}

/// Everything a chain's headers spell out that is neither closed nor a decl - it precedes the block of `let`s.
void Layout::headers() {
    for (auto h : heads_) {
        auto& hdr    = hdr_[h];
        auto& hdecls = hdr_decls_[h];
        auto visit   = [&](this auto&& self, const Def* d) -> void {
            if (!d || d->isa<Var>() || d->is_closed()) return;
            if (auto mut = isa_decl(d)) return (void)hdecls.emplace_back(mut);
            if (!hdr.emplace(d).second) return;
            deps(d, self);
        };
        auto slots = [&](this auto&& self, const Slot& s) -> void {
            visit(s.type);
            for (auto& c : s.comps)
                self(c);
        };
        auto& chain = chains_[h];
        for (auto m : chain) {
            visit(m->type()->dom());
            slots(ensure_binder(m)->root);
            if (m->is_set()) visit(m->filter());
        }
        if (auto last = chain.back(); !Pi::isa_cn(last->type())) visit(last->type()->codom());
    }
}

/*
 * P4: place
 */

void Layout::place() {
    if (opts_.mode == Dump::Local) return place_local();
    for (auto h : root_heads_)
        decl_block_[h] = &top_;

    for (auto S : scopes_) {
        const auto nest = Nest(S);
        auto sched      = Scheduler(nest);

        auto decls = open_decls_[S];
        std::ranges::stable_sort(decls, {}, [&](Def* m) { return nest[m] ? nest[m]->level() : 0u; });
        for (auto m : decls) {
            auto node = nest[m];
            if (!node) {
                unspell_.emplace(m);
                continue;
            }
            if (auto lam = m->isa_mut<Lam>(); lam && head_[lam] != lam) continue;
            auto dom = node->idom();
            if (auto b = blk(dom ? dom : node->inest())) decl_block_[m] = b;
        }

        for (auto d : cands_[S])
            if (auto b = blk(sched.smart(curr_[d], d))) place_[d] = b;

        for (auto h : scope_heads_[S])
            header_rule(h, block_of(h), &sched);
    }
}

void Layout::place_local() {
    for (auto h : root_heads_)
        decl_block_[h] = &top_;
    for (auto d : order3_) {
        if (!cand_set_.contains(d)) continue;
        auto lam = curr_[d] ? curr_[d]->isa_mut<Lam>() : nullptr;
        if (lam && decls_.contains(lam))
            if (auto b = block_of(head_of(lam))) place_[d] = b;
    }
    for (auto h : heads_)
        for (auto d : hdr_[h])
            if (cand_set_.contains(d)) header_.emplace(d);
}

/// The block whose mutable @p node stands for; `nullptr` if that one prints inline and hence has none.
Block* Layout::blk(const Nest::Node* node) {
    if (!node) return nullptr;
    auto m = node->mut();
    if (!m) return &top_;
    auto lam = m->isa_mut<Lam>();
    if (!lam || !decls_.contains(lam)) return nullptr;
    return block_of(head_of(lam));
}

Block* Layout::block_of(Lam* head) {
    if (auto i = block_idx_.find(head); i != block_idx_.end()) return i->second;
    auto p = decl_block_.find(head);
    if (p == decl_block_.end() || !chains_[head].back()->is_set()) return nullptr; // a bodyless Lam has no block
    auto& b                 = blocks_.emplace_back(Block{head, p->second, {}, nullptr});
    return block_idx_[head] = &b;
}

Lam* Layout::head_of(Lam* lam) const {
    auto i = head_.find(lam);
    return i == head_.end() ? lam : i->second;
}

bool Layout::at_or_below(const Block* b, const Block* anc) {
    for (; b; b = b->parent)
        if (b == anc) return true;
    return false;
}

/// A header Def that depends on the chain's own binders prints inline there; one that does not is hoisted above it.
void Layout::header_rule(Lam* h, Block* B, const Scheduler* sched) {
    if (!B) return;
    auto& chain = chains_[h];
    fe::Vector<const Var*> vars;
    for (auto m : chain)
        vars.append_range(ensure_binder(m)->vars);

    auto is_header_user = [&](const Use& u) {
        if (hdr_[h].contains(u.def())) return true;
        for (auto m : chain)
            if (u.def() == m->type() || (u.def() == m && u.index() == 0)) return true;
        return false;
    };

    for (auto d : hdr_[h]) {
        auto p = place_.find(d);
        if (std::ranges::any_of(vars, [&](auto v) { return d->has_free_var(v); })) {
            if (!cand_set_.contains(d)) continue;
            header_.emplace(d);
            if (p != place_.end() && std::ranges::all_of(sched->uses(d), is_header_user)) place_.erase(p);
        } else if (p != place_.end() && at_or_below(p->second, B)) {
            p->second = B->parent;
        }
    }
    for (auto m : hdr_decls_[h])
        if (auto i = decl_block_.find(m); i != decl_block_.end() && at_or_below(i->second, B)) {
            i->second = B->parent;
            if (auto bi = block_idx_.find(m); bi != block_idx_.end()) bi->second->parent = B->parent;
        }
}

/*
 * P5: order
 */

bool Layout::is_item(const Def* def) const {
    return place_.contains(def) || decl_block_.contains(def) || axm2group_.contains(def);
}

/// The items @p def reaches through what prints inline; @p self reached again means @p def is recursive.
void Layout::walk(const Def* def, DefSet& out, DefSet& seen, const Def* self, bool& recursive) {
    deps(def, [&](const Def* op) {
        if (!op) return;
        if (op == self) return (void)(recursive = true);
        if (auto g = axm2group_.find(op); g != axm2group_.end())
            return (void)out.emplace(axm_groups_[g->second].second.front());
        if (is_item(op)) return (void)out.emplace(op);
        if (auto mut = op->isa_mut(); mut && !descended_.contains(mut)) return;
        if (op->isa<Var>() || !seen.emplace(op).second) return;
        walk(op, out, seen, self, recursive);
    });
}

const DefSet& Layout::refs(const Def* item) {
    if (auto i = refs_.find(item); i != refs_.end()) return i->second;
    refs_.try_emplace(item); // in progress: a cycle back to @p item contributes nothing further
    DefSet out, seen;
    bool rec = false;
    if (auto g = axm2group_.find(item); g != axm2group_.end()) {
        for (auto axm : axm_groups_[g->second].second)
            walk(axm->type(), out, seen, item, rec);
    } else {
        walk(item, out, seen, item, rec);
        // A declaration's block is part of it: whatever its items reach, it reaches.
        if (auto lam = item->isa_mut<Lam>())
            if (auto b = block_idx_.find(lam); b != block_idx_.end())
                for (auto j : DefVec(out.begin(), out.end())) {
                    auto pj = place_.find(j);
                    auto dj = decl_block_.find(j);
                    if ((pj != place_.end() && pj->second == b->second)
                        || (dj != decl_block_.end() && dj->second == b->second))
                        for (auto& r = refs(j); auto d : r)
                            out.emplace(d);
                }
    }
    if (rec) self_rec_.emplace(item);
    return refs_[item] = std::move(out);
}

void Layout::order() {
    // A tail is spelled out where it is - it has no `let`, wherever else it may occur.
    for (auto& b : blocks_)
        if ((b.tail = chains_[b.owner].back()->body())) place_.erase(b.tail);
    if (roots_.size() == 1 && !isa_decl(roots_.front())) {
        top_.tail = roots_.front();
        place_.erase(top_.tail);
    }

    // Scheduler::early ignores mutable operands, so a `let` may float above a declaration it refers to.
    for (auto def : order3_) {
        auto p = place_.find(def);
        if (p == place_.end()) continue;
        for (auto j : refs(def))
            if (auto d = decl_block_.find(j); d != decl_block_.end() && at_or_below(d->second, p->second))
                p->second = d->second;
    }

    // A decl that has no block of its own is a λ-expression - which cannot refer to itself.
    for (auto def : order3_) {
        auto mut = isa_decl(def);
        if (!mut || !decls_.contains(mut) || decl_block_.contains(mut)) continue;
        if (auto lam = mut->isa_mut<Lam>(); lam && head_of(lam) != lam) continue;
        DefSet out, seen;
        bool rec = false;
        walk(mut, out, seen, mut, rec);
        if (rec) unspell_.emplace(mut);
    }

    absl::flat_hash_map<Block*, DefVec> pending;
    for (auto& [mod, group] : axm_groups_)
        pending[&top_].emplace_back(group.front());
    for (auto def : order3_)
        if (auto p = place_.find(def); p != place_.end())
            pending[p->second].emplace_back(def);
        else if (auto d = decl_block_.find(def); d != decl_block_.end())
            pending[d->second].emplace_back(def);
    tarjan(top_, pending[&top_]);
    for (auto& b : blocks_)
        tarjan(b, pending[&b]);
}

static Item let_item(const Def* def) {
    auto item = Item();
    item.tag  = Item::Tag::Let;
    item.def  = def;
    return item;
}

Item Layout::axms_item(size_t group) const {
    auto item = Item();
    item.tag  = Item::Tag::Axms;
    item.axms = axm_groups_[group].second;
    item.mod  = axm_groups_[group].first;
    return item;
}

/// Emits the items of @p block callee-first: an SCC of several declarations becomes one `and` chain.
void Layout::tarjan(Block& block, const DefVec& nodes) {
    DefSet node_set(nodes.begin(), nodes.end());
    DefMap<u32> index, low;
    DefSet on_stack;
    DefVec stack;
    u32 counter = 0;

    auto emit = [&](DefVec scc) {
        std::ranges::sort(scc, {}, [&](const Def* d) { return seq_.contains(d) ? seq_[d] : 0u; });
        if (scc.size() == 1) {
            auto d = scc.front();
            if (auto p = place_.find(d); p != place_.end()) return (void)block.items.emplace_back(let_item(d));
            if (auto g = axm2group_.find(d); g != axm2group_.end())
                return (void)block.items.emplace_back(axms_item(g->second));
        }
        auto item = Item();
        item.tag  = Item::Tag::Decl;
        for (auto d : scc) {
            if (auto g = axm2group_.find(d); g != axm2group_.end()) {
                block.items.emplace_back(axms_item(g->second));
            } else if (place_.contains(d)) {
                place_.erase(d); // a `let` cannot take part in a cycle, so it prints where it is used
            } else {
                auto mut = const_cast<Def*>(d);
                item.muts.emplace_back(mut);
                if (!mut->isa<Lam>() && (scc.size() > 1 || self_rec_.contains(mut))) item.rec = true;
            }
        }
        if (item.muts.empty()) return;
        std::ranges::stable_partition(item.muts, [](Def* m) { return m->is_external(); }); // `and` takes no modifiers
        block.items.emplace_back(std::move(item));
    };

    auto strong = [&](this auto&& self, const Def* v) -> void {
        index[v] = low[v] = counter++;
        stack.emplace_back(v);
        on_stack.emplace(v);
        auto succs = DefVec(refs(v).begin(), refs(v).end());
        std::ranges::sort(succs, {}, [&](const Def* d) { return seq_.contains(d) ? seq_[d] : 0u; });
        for (auto w : succs) {
            if (!node_set.contains(w)) continue;
            if (!index.contains(w)) {
                self(w);
                low[v] = std::min(low[v], low[w]);
            } else if (on_stack.contains(w)) {
                low[v] = std::min(low[v], index[w]);
            }
        }
        if (low[v] == index[v]) {
            DefVec scc;
            const Def* w;
            do {
                w = stack.back();
                stack.pop_back();
                on_stack.erase(w);
                scc.emplace_back(w);
            } while (w != v);
            emit(std::move(scc));
        }
    };

    for (auto v : nodes)
        if (!index.contains(v)) strong(v);
}

/*
 * P6: spell binders
 */

void Layout::spell() {
    // Outermost first: a Lam inside a `fun` must not hide that one's `return`.
    fe::Vector<Lam*> lams;
    for (auto def : order3_ | std::views::reverse)
        if (auto lam = def->isa_mut<Lam>(); lam && descended_.contains(lam)) lams.emplace_back(lam);
    fe::Vector<const Var*> funs;
    for (auto lam : lams) {
        auto b = ensure_binder(lam);
        auto k = Kind::Lam;
        if (fun_shape(lam) && !b->root.used && std::ranges::none_of(funs, [&](auto v) { return lam->has_free_var(v); }))
            k = Kind::Fun;
        else if (Lam::isa_cn(lam))
            k = Kind::Con;
        kinds_[lam] = k;
        if (k == Kind::Fun)
            if (auto v = lam->has_var()) funs.emplace_back(v);
    }

    for (auto& b : binder_store_) {
        auto lam = b.mut->isa_mut<Lam>();
        auto fun = std::ranges::any_of(b.vars, [&](auto v) {
            auto l = v->binder()->template isa_mut<Lam>();
            return l && kinds_.contains(l) && kinds_[l] == Kind::Fun;
        });
        if (lam && !lam->has_var()) { // a bodyless Lam spells its domain as a telescope
            fun = kinds_.contains(lam) && kinds_[lam] == Kind::Fun;
            expand(b.root);
            b.root.pattern = b.root.is_tuple();
            if (fun && b.root.comps.size() == 2) {
                expand(b.root.comps[0]);
                b.root.comps[0].pattern = b.root.comps[0].is_tuple();
                b.root.comps[1].ret     = true;
            }
            continue;
        }
        if (fun) {
            expand(b.root);
            if (b.root.comps.size() == 2) {
                auto& params        = b.root.comps[0];
                b.root.pattern      = true;
                b.root.comps[1].ret = true;
                expand(params);
                decide(params);
                params.pattern = params.is_tuple(); // a `fun` always takes its parameters apart
                continue;
            }
        }
        decide(b.root);
    }
}

bool Layout::decide(Slot& s) {
    bool any = false;
    for (auto& c : s.comps) {
        auto sub = decide(c);
        any |= sub || c.used || user_named(c.def);
    }
    s.pattern = s.expanded && any;
    if (s.owner && s.owner->mut->isa<Seq>()) s.pattern = s.is_tuple(); // axes are always spelled out
    return s.pattern;
}

/*
 * queries
 */

const Block* Layout::block(Lam* head) const {
    auto i = block_idx_.find(head);
    return i == block_idx_.end() ? nullptr : i->second;
}

const Binder* Layout::binder(Def* mut) const {
    if (opts_.mode != Dump::Expr) {
        auto var = mut->has_var();
        auto key = var ? find(var)->binder() : mut;
        if (auto i = binders_.find(key); i != binders_.end()) return i->second;
    }
    return structural(mut);
}

const Slot* Layout::resolve(const Def* def, const Lam* ctx) const {
    if (auto var = def->isa<Var>()) {
        const Binder* b = nullptr;
        if (is_ctx_var(var, ctx)) {
            // The dom Sigma spells its own fields as long as the Lam keeps its parameter whole.
            auto lb = binder(const_cast<Lam*>(ctx));
            if (var == ctx->type()->has_var() || (lb && lb->root.pattern)) b = lb;
        }
        if (!b) b = binder(var->binder());
        return b ? &b->root : nullptr;
    }
    if (auto ex = def->isa<Extract>())
        if (auto l = Lit::isa(ex->index()))
            if (auto p = resolve(ex->tuple(), ctx)) {
                auto base = def;
                while (auto e = base->isa<Extract>())
                    base = e->tuple();
                auto field = base->as<Var>()->binder()->isa<Sigma>() != nullptr;
                auto n     = Lit::isa(Idx::isa(ex->index()->type()));
                if ((field ? p->expanded : p->pattern) && n && p->comps.size() == *n && *l < *n) return &p->comps[*l];
            }
    return nullptr;
}

Sym Layout::name(const Def* def, const Lam* ctx) const {
    if (def->is_external() || (def->isa<Lam>() && !def->is_set())) return def->sym();
    if (auto i = picked_.find(def); i != picked_.end()) return i->second;
    if (auto slot = resolve(def, ctx)) return slot_name(*slot);
    if (opts_.mode == Dump::Expr) {
        if (isa_decl(def) || def->isa<Var>() || (!def->isa_mut() && !def->is_closed() && !spells_inline(def)))
            return world_.sym(Names::fallback(def));
        return Sym();
    }
    if (names_ && (place_.contains(def) || decl_block_.contains(def))) return picked_[def] = names_->pick(def);
    if ((isa_decl(def) && !descended_.contains(def)) || def->isa<Var>()) return world_.sym(Names::fallback(def));
    return Sym();
}

Sym Layout::slot_name(const Slot& s) const {
    if (s.sym || !names_) return s.sym;
    if (s.ret) return s.sym = world_.sym("return");
    if (s.used || (!s.pattern && (s.field || user_named(s.def)))) return s.sym = names_->pick(s.def);
    return Sym();
}

bool Layout::spellable(const Match* match) const {
    if (opts_.mode != Dump::Expr) return matches_.contains(match);
    auto tt = world_.lit_tt();
    return match->num_arms() != 0 && std::ranges::all_of(match->arms(), [&](const Def* arm) {
               auto lam = arm->isa_mut<Lam>();
               return lam && lam->is_set() && lam->filter() == tt;
           });
}

Layout::Kind Layout::kind(const Lam* lam) const {
    if (opts_.mode != Dump::Expr) {
        auto i = kinds_.find(lam);
        return i == kinds_.end() ? Kind::Lam : i->second;
    }
    auto [i, fresh] = structural_kinds_.try_emplace(lam, Kind::Lam);
    if (fresh) i->second = fun_shape(lam) ? Kind::Fun : Lam::isa_cn(lam) ? Kind::Con : Kind::Lam;
    return i->second;
}

fe::Vector<Lam*> Layout::chain(Lam* lam) const {
    if (auto i = chains_.find(lam); i != chains_.end()) return i->second;
    return {lam};
}

/*
 * Dump::Expr: structural binders
 */

const Binder* Layout::structural(Def* mut) const {
    auto [i, fresh] = structural_.try_emplace(mut, nullptr);
    if (!fresh) return i->second;
    auto& b      = structural_store_.emplace_back();
    i->second    = &b;
    b.mut        = mut;
    b.root.owner = &b;
    b.root.def   = mut->has_var();
    b.root.type  = binder_type(mut);
    b.root.used  = b.root.def != nullptr;
    if (b.root.def) {
        b.vars.emplace_back(mut->has_var());
        b.root.sym = world_.sym(Names::fallback(b.root.def));
    }
    auto lam = mut->isa_mut<Lam>();
    auto fun = lam && kind(lam) == Kind::Fun;
    expand_structural(b.root, fun);
    if (fun && b.root.comps.size() == 2) {
        b.root.pattern      = true;
        b.root.comps[1].ret = true;
        b.root.comps[1].sym = world_.sym("return");
    }
    return &b;
}

/// Components exist only where the World hands out the projection; a component the World lacks stays `_: T`.
void Layout::expand_structural(Slot& s, bool force) const {
    auto root = s.owner;
    if (auto seq = root ? root->mut->isa<Seq>() : nullptr) {
        auto r = seq->shape().rank();
        if (!r || *r <= 1) return;
        for (auto k : std::views::iota(nat_t(0), *r)) {
            auto& c = s.comps.emplace_back();
            if ((c.def = s.def ? s.def->proj(*r, k) : nullptr))
                c.used = true, c.sym = world_.sym(Names::fallback(c.def));
        }
        s.expanded = s.pattern = true;
        return;
    }
    auto type = s.type;
    if (!type) return;
    auto n   = num_binders(type);
    auto own = root && root->mut == type;
    if (n <= 1 || (isa_decl(type) && !own)) return;
    auto [sigma, sv] = type->isa_binder<Sigma>();
    if (own) sv = nullptr; // a Sigma's own fields may well refer to each other
    auto has_lit = world_.lit_nat(n) != nullptr;
    std::vector<Slot> comps;
    for (auto i : std::views::iota(nat_t(0), n)) {
        auto& c = comps.emplace_back();
        c.def   = has_lit && s.def ? s.def->proj(n, i) : nullptr;
        if (c.def) {
            c.type = c.def->type();
            c.used = true;
            c.sym  = world_.sym(Names::fallback(c.def));
        } else {
            c.type = comp_type(type, i);
            if (!force && (!c.type || (sv && c.type->has_free_var(sv)))) return;
        }
    }
    if (!force && std::ranges::none_of(comps, [](auto& c) { return c.def != nullptr; })) return;
    s.comps    = std::move(comps);
    s.expanded = s.pattern = true;
    for (auto& c : s.comps)
        expand_structural(c, false);
}

} // namespace mim::dump
