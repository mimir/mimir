#include "mim/plug/omp/phase/outline.h"

#include <algorithm>

#include <fe/vector.h>

#include <mim/lam.h>

#include <mim/plug/core/core.h>
#include <mim/plug/cps/cps.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/omp/omp.h"

namespace mim::plug::omp::phase {

namespace {

bool has_mem(const Def* type) {
    if (Axm::isa<mem::M>(type)) return true;
    if (auto sigma = type->isa<Sigma>()) return std::ranges::any_of(sigma->ops(), [](auto op) { return has_mem(op); });
    if (auto arr = type->isa<Arr>()) return has_mem(arr->body());
    return false;
}

} // namespace

/// The values @p body takes from its enclosing function: every largest subterm that is open but independent of
/// the mutables the kernel is made of.
/// Those are all mutables reachable from the body without crossing a variable; the kernel rebuilds them, and
/// types, over the captured values.
std::optional<DefVec> Outline::captures(Lam* body) {
    auto seen        = DefSet();
    auto inside_muts = DefSet{body};
    auto inside_vars = fe::Vector<const Var*>();
    if (auto var = body->has_var()) inside_vars.emplace_back(var);

    auto reach = [&](auto& self, const Def* def) -> void {
        if (def->is_closed() || def->isa<Var>() || !seen.emplace(def).second) return;
        if (auto mut = def->isa_mut()) {
            inside_muts.emplace(mut);
            if (auto var = mut->has_var()) inside_vars.emplace_back(var);
        }
        for (auto dep : def->deps())
            self(self, dep);
    };
    for (auto op : body->ops())
        reach(reach, op);

    auto is_inside = [&](const Def* def) {
        return std::ranges::any_of(inside_vars, [def](auto var) { return def->has_free_var(var); })
            || std::ranges::any_of(def->local_muts(), [&](auto mut) { return inside_muts.contains(mut); });
    };

    auto result = DefVec();
    bool ok     = true;
    seen.clear();
    auto visit = [&](auto& self, const Def* def) -> void {
        if (!ok || def->is_closed() || !seen.emplace(def).second) return;

        if (def->isa_mut() || !def->is_term() || is_inside(def)) {
            for (auto dep : def->deps())
                self(self, dep);
        } else if (def->isa<Var>() || has_mem(def->type())) {
            log().d("cannot capture `{}: {}`", def, def->type());
            ok = false;
        } else {
            result.emplace_back(def);
        }
    };
    for (auto op : body->ops())
        visit(visit, op);

    if (!ok) return std::nullopt;
    return result;
}

const Def* Outline::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);

    auto par_for = Axm::isa<omp::par_for>(app);
    if (!par_for) return RWPhase::rewrite_imm_App(app);

    auto& w                                      = new_world();
    auto [old_body, old_args]                    = par_for->uncurry_args<2>();
    auto [old_mem, old_begin, old_end, old_step] = old_args->projs<4>();
    auto old_body_lam                            = old_body->isa_mut<Lam>();
    if (!old_body_lam) old_body_lam = Lam::eta_expand(old_body);

    auto new_mem   = rewrite(old_mem);
    auto new_begin = rewrite(old_begin);
    auto new_end   = rewrite(old_end);
    auto new_step  = rewrite(old_step);
    auto mem_ty    = new_mem->type();
    auto idx_ty    = new_begin->type();

    // The body over the whole range as one direct-style call; cps2ds wants a continuation of two parameters.
    auto sequential = [&](std::string_view why) {
        log().w("{}: running sequentially since {}", app, why);
        auto seq         = w.mut_con(Defs{w.sigma({mem_ty, idx_ty, idx_ty}), w.cn(mem_ty)})->set("omp_seq");
        auto [args, ret] = seq->vars<2>();
        auto [m, lo, hi] = args->projs<3>();
        seq->app(true, rewrite(old_body_lam), w.tuple({m, lo, hi, ret}));
        return w.app(cps::op_cps2ds_dep(seq), w.tuple({new_mem, new_begin, new_end}));
    };

    auto captured = captures(old_body_lam);
    if (!captured) return sequential("its body uses memory or a whole variable of the enclosing function");

    auto n      = captured->size();
    auto env    = w.tuple(DefVec(n, [&](size_t i) { return rewrite((*captured)[i]); }));
    auto env_ty = env->type();
    auto i64    = w.type_i64();
    auto kernel = w.mut_con(Defs{mem_ty, w.call<mem::Ptr>(Defs{env_ty, w.lit_nat_0()}), i64, i64, w.cn(mem_ty)})
                      ->set("omp_kernel");
    auto [k_mem, k_env_ptr, k_lo, k_hi, k_ret] = kernel->vars<5>();

    auto k_cur       = k_mem;
    const Def* k_env = nullptr;
    if (n != 0) {
        auto [m, e] = w.call<mem::load>(Defs{k_mem, k_env_ptr})->projs<2>();
        k_cur       = m;
        k_env       = e;
    }

    auto mod    = Idx::isa(new_begin->type());
    auto narrow = [&](const Def* i) { return w.call(core::conv::u, mod, i); };

    push();
    for (size_t i = 0; i != n; ++i)
        map((*captured)[i], k_env->proj(n, i));
    map(old_body_lam->var(), w.tuple({k_cur, narrow(k_lo), narrow(k_hi), k_ret}));
    auto new_body = rewrite(old_body_lam->body());
    pop();
    kernel->set({w.lit_ff(), new_body});

    if (!kernel->free_vars().empty())
        return sequential("the type of a captured value depends on the enclosing function");

    // The gid keeps two kernels' symbols apart.
    auto sym = kernel->unique_name();
    kernel->set<true>(sym)->externalize();

    auto widen  = [&](const Def* i) { return w.call(core::conv::u, w.lit_nat_0(), i); };
    auto name   = w.tuple(DefVec(sym.size(), [&](size_t i) { return w.lit(w.type_i8(), sym[i]); }));
    auto launch = w.app(w.app(w.annex<omp::launch>(), Defs{env_ty, w.lit_nat(sym.size())}), name);
    return w.app(launch, w.tuple({new_mem, env, widen(new_begin), widen(new_end), widen(new_step)}));
}

} // namespace mim::plug::omp::phase
