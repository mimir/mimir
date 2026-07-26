#pragma once

#include <deque>
#include <format>
#include <optional>
#include <ostream>
#include <string>

#include <absl/container/btree_set.h>

#include <mim/driver.h>

#include <mim/be/emitter.h>

#include <mim/plug/clos/clos.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>
#include <mim/plug/vec/vec.h>

// Lessons learned:
// * **Always** follow all ops - even if you actually want to ignore one.
//   Otherwise, you might end up with an incorrect schedule.
//   This was the case for an Extract of type Mem.
//   While we want to ignore the value obtained from that, since there is no Mem value in LLVM,
//   we still want to **first** recursively emit code for its operands and **then** ignore the Extract itself.
// * i1 has a different meaning in LLVM then in Mim:
//      * Mim: {0,  1} = i1
//      * LLVM:   {0, -1} = i1
//   This is a problem when, e.g., using an index of type i1 as LLVM thinks like this:
//   getelementptr ..., i1 1 == getelementptr .., i1 -1
namespace mim {

class World;

namespace plug::ll {

namespace math = mim::plug::math;
namespace mem  = mim::plug::mem;

namespace detail {
inline const char* math_suffix(const Def* type) {
    if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 32: return "f";
            case 64: return "";
        }
    }
    fe::throwf("unsupported floating point type '{}'", type);
}

inline const char* llvm_suffix(const Def* type) {
    if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 16: return ".f16";
            case 32: return ".f32";
            case 64: return ".f64";
        }
    }
    fe::throwf("unsupported floating point type '{}'", type);
}

// [%mem.M 0, T] => T
// TODO there may be more instances where we have to deal with this trickery
inline const Def* isa_mem_sigma_2(const Def* type) {
    if (auto sigma = type->isa<Sigma>())
        if (sigma->num_ops() == 2 && Axm::isa<mem::M>(sigma->op(0))) return sigma->op(1);
    return {};
}
} // namespace detail

struct BB {
    BB()                    = default;
    BB(const BB&)           = delete;
    BB(BB&& other) noexcept = default;
    BB& operator=(BB other) noexcept { return swap(*this, other), *this; }

    std::deque<std::ostringstream>& head() { return parts[0]; }
    std::deque<std::ostringstream>& body() { return parts[1]; }
    std::deque<std::ostringstream>& tail() { return parts[2]; }

    template<class... Args>
    inline std::string assign(std::string_view name, std::format_string<Args...> s, Args&&... args) {
        auto& os = body().emplace_back();
        std::print(os, "{} = ", name);
        std::print(os, s, std::forward<Args>(args)...);
        return std::string(name);
    }

    template<class... Args>
    inline void tail(std::format_string<Args...> s, Args&&... args) {
        std::print(tail().emplace_back(), s, std::forward<Args>(args)...);
    }

    friend inline void swap(BB& a, BB& b) noexcept {
        using std::swap;
        swap(a.phis, b.phis);
        swap(a.parts, b.parts);
    }

    DefMap<std::deque<std::pair<std::string, std::string>>> phis;
    std::array<std::deque<std::ostringstream>, 3> parts;
};

class Emitter;

/// The heavy, target-independent emitter methods are compiled once into `libmim_ll` (see ll.cpp).
/// `libmim_ll_nvptx` reaches them through these `extern "C"` shims instead of recompiling the bodies;
/// Emitter caches the pointers via `GET_FUN_PTR` in its constructor.
extern "C" {
MIM_EXPORT void mim_ll_convert(Emitter&, const Def*, bool simd, std::string& res);
MIM_EXPORT void mim_ll_finalize(Emitter&);
MIM_EXPORT void mim_ll_emit_epilogue(Emitter&, Lam*);
MIM_EXPORT void mim_ll_emit_bb(Emitter&, BB&, const Def*, std::string& res);
}

class Emitter : public mim::Emitter<std::string, std::string, BB, Emitter> {
public:
    using Super = mim::Emitter<std::string, std::string, BB, Emitter>;

    Emitter(World& world, std::string name, std::ostream& ostream)
        : Super(world, name, ostream) {
        auto& driver = world.driver();
        // Ensure libmim_ll is loaded so the shims below resolve (e.g. when a derived backend like
        // ll_nvptx uses us). Loading merely registers %ll.emit; it does not run it.
        if (auto ll = driver.sym("ll"); !driver.is_loaded(ll)) driver.load(ll);
        convert_       = driver.GET_FUN_PTR("ll", mim_ll_convert);
        finalize_      = driver.GET_FUN_PTR("ll", mim_ll_finalize);
        emit_epilogue_ = driver.GET_FUN_PTR("ll", mim_ll_emit_epilogue);
        emit_bb_       = driver.GET_FUN_PTR("ll", mim_ll_emit_bb);
    }

    bool is_valid(std::string_view s) { return !s.empty(); }
    void start() override;
    void emit_imported(Lam*);
    virtual std::string prepare();

    // Thin forwarders into `libmim_ll`; the real bodies (`*_impl`) live in ll.cpp.
    virtual void emit_epilogue(Lam* lam) { emit_epilogue_(*this, lam); }
    void finalize() { finalize_(*this); }
    std::string emit_bb(BB& bb, const Def* def) {
        std::string res;
        emit_bb_(*this, bb, def, res);
        return res;
    }

    virtual inline std::optional<std::string> isa_targetspecific_intrinsic(BB&, const Def*) { return std::nullopt; }

    template<class... Args>
    void declare(std::format_string<Args...> s, Args&&... args) {
        std::ostringstream decl;
        decl << "declare ";
        std::print(decl, s, std::forward<Args>(args)...);
        decls_.emplace(decl.str());
    }

    /// How the C runtime wrappers (see `rt/mim_rt.c`, compiled to `mim_rt.ll`) reach the output.
    enum class Rt {
        embed, ///< Splice the wrapper IR into the emitted module so it is self-contained.
        ext,   ///< Only `declare` the wrappers; the runtime is linked in externally.
    };

    void rt_mode(Rt rt) { rt_ = rt; }
    /// Provides the textual LLVM IR of the runtime module to splice in `Rt::embed` mode.
    void rt_module(std::string ll) { rt_module_ = std::move(ll); }

    /// Declares a runtime wrapper @p sig (implemented in `rt/mim_rt.c`) and records that the
    /// runtime is required by this module.
    /// In `Rt::ext` mode the declaration is emitted like any other `declare`.
    /// In `Rt::embed` mode the wrapper's *definition* is spliced into the output, so emitting a
    /// `declare` as well would be a redefinition — hence it is suppressed here.
    template<class... Args>
    void declare_rt(std::format_string<Args...> sig, Args&&... args) {
        rt_used_ = true;
        if (rt_ == Rt::ext) declare(sig, std::forward<Args>(args)...);
    }

protected:
    std::string id(const Def*, bool force_bb = false) const;
    virtual std::string convert(const Def* type, bool simd = true) {
        std::string res;
        convert_(*this, type, simd, res);
        return res;
    }
    std::string convert_ret_pi(const Pi*);

    /// Registers @p arg as an incoming phi value for @p phi in @p callee, coming from predecessor @p pred.
    void emit_phi(Lam* callee, const Def* phi, std::string arg, Lam* pred) {
        lam2bb_[callee].phis[phi].emplace_back(std::move(arg), id(pred, true));
        locals_[phi] = id(phi);
    }

    /// Wires all non-`%mem.M` arguments of @p app into @p callee's phis, coming from predecessor @p pred.
    void emit_phi_args(Lam* callee, const App* app, Lam* pred) {
        size_t n = callee->num_tvars();
        for (size_t i = 0; i != n; ++i)
            if (auto arg = emit_unsafe(app->arg(n, i)); !arg.empty()) {
                auto phi = callee->var(n, i);
                if (Axm::isa<mem::M>(phi->type())) continue;
                emit_phi(callee, phi, std::move(arg), pred);
            }
    }

    /// Emits the storage backing a `%mem.slot` of type @p pointee and yields the pointer value.
    /// The generic backend allocates on the stack; targets may override (e.g. a global in a specific address space).
    /// Kept inline on purpose so `Emitter` retains no vtable key function (else its vtable would live in a single
    /// module and break derived backends loaded from a separate plugin).
    virtual std::string emit_slot(BB& bb, const App* app, const Def* pointee, const Def* /*addr_space*/) {
        auto v_ptr = "%" + app->unique_name() + ".slot";
        std::print(bb.body().emplace_back(), "{} = alloca {}", v_ptr, convert(pointee, false));
        return v_ptr;
    }

    absl::btree_set<std::string> decls_;
    std::ostringstream type_decls_;
    std::ostringstream vars_decls_;
    std::ostringstream func_decls_;
    std::ostringstream func_impls_;
    LamMap<const Def*> simd_phi_;

    Rt rt_        = Rt::embed;
    bool rt_used_ = false;
    std::string rt_module_;

private:
    // Real implementations; defined in ll.cpp and exported via the `mim_ll_*` shims above.
    std::string convert_impl(const Def*, bool simd);
    void finalize_impl();
    void emit_epilogue_impl(Lam*);
    std::string emit_bb_impl(BB&, const Def*);

    decltype(&mim_ll_convert) convert_             = nullptr;
    decltype(&mim_ll_finalize) finalize_           = nullptr;
    decltype(&mim_ll_emit_epilogue) emit_epilogue_ = nullptr;
    decltype(&mim_ll_emit_bb) emit_bb_             = nullptr;

    friend void mim_ll_convert(Emitter&, const Def*, bool, std::string&);
    friend void mim_ll_finalize(Emitter&);
    friend void mim_ll_emit_epilogue(Emitter&, Lam*);
    friend void mim_ll_emit_bb(Emitter&, BB&, const Def*, std::string&);
};

/*
 * convert
 */

inline static std::optional<std::pair<nat_t, const Def*>> is_simd(const Def* type) {
    if (auto arr = type->isa<Arr>()) {
        if (auto l = Lit::isa(arr->arity())) {
            if (arr->body()->isa<Nat>() || Idx::isa(arr->body()) || Axm::isa<math::F>(arr->body()))
                return std::pair{*l, arr->body()};
        }
    }
    return {};
}

inline static std::optional<std::pair<nat_t, const Def*>> is_simd_aggregate(Defs types) {
    if (std::ranges::all_of(types, [&](auto i) { return i == types[0]; })) {
        if (types[0]->isa<Nat>() || Idx::isa(types[0]) || Axm::isa<math::F>(types[0]))
            return std::pair{types.size(), types[0]};
    }

    return {};
}

inline static const Def* find_common_simd_src(const App* app) {
    const Def* common_src = nullptr;
    size_t lane           = 0;
    for (auto arg : app->args()) {
        if (Axm::isa<mem::M>(arg->type())) continue;
        auto extract = arg->isa<Extract>();
        if (!extract || !is_simd(extract->tuple()->type())) return nullptr;
        // Only devectorized args - lane i in position i - may forward the whole vector;
        // anything else (e.g. a Select with a non-literal index) must stay scalar.
        if (auto index = Lit::isa(extract->index()); !index || *index != lane++) return nullptr;
        if (!common_src)
            common_src = extract->tuple();
        else if (common_src != extract->tuple())
            return nullptr;
    }
    if (common_src) {
        auto simd = is_simd(common_src->type());
        if (!simd || simd->first != lane) return nullptr;
    }
    return common_src;
}

inline std::string Emitter::id(const Def* def, bool force_bb /*= false*/) const {
    if (auto global = def->isa<Global>()) return "@" + global->unique_name();

    if (auto lam = def->isa_mut<Lam>(); lam && !force_bb) {
        if (lam->type()->ret_pi()) {
            if (lam->is_external() || !lam->is_set())
                return std::string("@") + lam->sym().str(); // TODO or use is_internal or sth like that?
            return std::string("@") + lam->unique_name();
        }
    }

    return std::string("%") + def->unique_name();
}

inline std::string Emitter::convert_ret_pi(const Pi* pi) {
    auto dom = mem::strip_mem_ty(pi->dom());
    if (dom == world().sigma()) return "void";
    return convert(dom);
}

/*
 * emit
 */

inline void Emitter::start() {
    Super::start();

    // Splice the runtime wrapper module first (it carries the module's target triple/datalayout).
    if (rt_used_ && rt_ == Rt::embed) {
        if (rt_module_.empty())
            fe::throwf("ll backend: `-X ll:rt=embed` needs the runtime module `mim_rt.ll`, but it "
                       "was not found (build with clang / `MIM_BUILD_LL_RUNTIME=ON`, or use `-X ll:rt=extern`)");
        ostream() << rt_module_ << '\n';
    }

    ostream() << type_decls_.str() << '\n';
    for (auto&& decl : decls_)
        ostream() << decl << '\n';
    ostream() << func_decls_.str() << '\n';
    ostream() << vars_decls_.str() << '\n';
    ostream() << func_impls_.str() << '\n';
}

inline void Emitter::emit_imported(Lam* lam) {
    // TODO merge with declare method
    std::print(func_decls_, "declare {} {}(", convert_ret_pi(lam->type()->ret_pi()), id(lam));

    auto doms = lam->doms();
    for (auto sep = ""; auto dom : doms.view().rsubspan(1)) {
        if (Axm::isa<mem::M>(dom)) continue;
        std::print(func_decls_, "{}{}", sep, convert(dom));
        sep = ", ";
    }

    std::print(func_decls_, ")\n");
}

inline std::string Emitter::prepare() {
    auto internal = root()->is_external() ? "" : "internal ";
    auto ret_t    = convert_ret_pi(root()->type()->ret_pi());
    std::print(func_impls_, "define {} {} {}(", internal, ret_t, id(root()));

    auto vars = root()->vars();
    for (auto sep = ""; auto var : vars.view().rsubspan(1)) {
        if (Axm::isa<mem::M>(var->type())) continue;
        if (auto sigma = var->type()->isa<Sigma>(); sigma && sigma->num_ops() == 0) continue;
        if (auto arr = var->type()->isa<Arr>())
            if (is_simd(arr->body())) convert(arr->body()); // pre-add input vector to cache
        auto name    = id(var);
        locals_[var] = name;
        std::print(func_impls_, "{}{} {}", sep, convert(var->type()), name);
        sep = ", ";
    }

    std::print(func_impls_, ") {{\n");
    return root()->unique_name();
}

} // namespace plug::ll
} // namespace mim
