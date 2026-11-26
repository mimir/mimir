#include "mim/plug/mach/libm.h"

#include <mim/plug/math/math.h>

namespace mim::plug::mach {

namespace {

/// The libm name of @p app without the format suffix and its arity, or an empty name.
std::pair<std::string, size_t> libm(const App* app) {
    if (auto exp = Axm::isa<math::exp>(app)) {
        if (exp.id() == math::exp::lBB || exp.id() == math::exp::LBB) return {};
        auto name = std::string(exp.sub() & sub_t(math::exp::log) ? "log" : "exp");
        if (exp.sub() & sub_t(math::exp::bin)) name += "2";
        if (exp.sub() & sub_t(math::exp::dec)) name += "10";
        return {name, 1};
    }
    if (auto tri = Axm::isa<math::tri>(app)) {
        auto name = std::string(tri.sub() & sub_t(math::tri::a) ? "a" : "");
        switch (math::tri((fe::to_underlying(tri.id()) & 0x3) | Annex::base<math::tri>())) {
            case math::tri::sin: name += "sin"; break;
            case math::tri::cos: name += "cos"; break;
            case math::tri::tan: name += "tan"; break;
            default: return {};
        }
        if (tri.sub() & sub_t(math::tri::h)) name += "h";
        return {name, 1};
    }
    if (Axm::isa<math::pow>(app)) return {"pow", 2};
    if (auto rt = Axm::isa<math::rt>(app); rt && rt.id() == math::rt::cb) return {"cbrt", 1};
    if (auto er = Axm::isa<math::er>(app)) return {er.id() == math::er::f ? "erf" : "erfc", 1};
    if (auto gamma = Axm::isa<math::gamma>(app)) return {gamma.id() == math::gamma::t ? "tgamma" : "lgamma", 1};
    if (auto round = Axm::isa<math::round>(app)) {
        switch (round.id()) {
            case math::round::f: return {"floor", 1};
            case math::round::c: return {"ceil", 1};
            case math::round::r: return {"round", 1};
            case math::round::t: return {"trunc", 1};
        }
    }
    if (auto arith = Axm::isa<math::arith>(app); arith && arith.id() == math::arith::rem) return {"fmod", 2};
    if (auto extrema = Axm::isa<math::extrema>(app)) {
        if (extrema.id() == math::extrema::fmin) return {"fmin", 2};
        if (extrema.id() == math::extrema::fmax) return {"fmax", 2};
    }
    return {};
}

} // namespace

const Def* Libm::rewrite_imm_App(const App* app) {
    if (liftable()) {
        auto [name, n] = libm(app);
        auto width     = math::isa_f(app->type());
        if (!name.empty() && (width == 32 || width == 64)) {
            auto& w    = new_world();
            auto type  = rewrite(app->type());
            auto args  = DefVec(n, [&](size_t i) { return rewrite(app->arg(n, i)); });
            auto sym   = w.sym(*width == 32 ? name + "f" : name);
            auto& decl = decls_[sym];
            if (!decl) {
                auto doms = DefVec(n, [&](size_t) { return type; });
                doms.emplace_back(w.cn(type));
                decl = w.mut_con(doms)->set(sym);
            }
            return lift(decl, args, type);
        }
    }
    return DS2CPS::rewrite_imm_App(app);
}

} // namespace mim::plug::mach
