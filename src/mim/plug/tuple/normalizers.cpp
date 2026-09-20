#include <mim/tuple.h>
#include <mim/world.h>

#include "mim/plug/tuple/tuple.h"

namespace mim::plug::tuple {

const Def* normalize_cat(const Def*, const Def* callee, const Def* arg) {
    auto [a, b] = arg->projs<2>();
    auto [n, m] = callee->as<App>()->decurry()->args<2>([](auto def) { return Lit::isa(def); });
    if (n && *n == 0) return b;
    if (m && *m == 0) return a;
    return n && m ? Tuple::cat(*n, *m, a, b) : nullptr;
}

const Def* normalize_contains(const Def* type, const Def*, const Def* arg) {
    auto& w      = type->world();
    auto [xs, x] = arg->projs<2>();

    if (auto mut_pack = xs->isa_mut<Pack>()) {
        if (auto imm = mut_pack->immutabilize())
            xs = imm;
        else
            return nullptr;
    }

    if (auto tuple = xs->isa<Tuple>()) {
        for (auto op : tuple->ops())
            if (op == x) return w.lit_tt();

        return tuple->is_closed() ? w.lit_ff() : nullptr;
    }

    if (auto pack = xs->isa<Pack>()) {
        if (pack->body() == x) return w.lit_tt();
        return pack->is_closed() ? w.lit_ff() : nullptr;
    }

    return nullptr;
}

const Def* normalize_zip(const Def* type, const Def* c, const Def* arg) {
    auto& w                    = type->world();
    auto callee                = c->as<App>();
    auto is_os                 = callee->arg();
    auto [n_i, Is, n_o, Os, f] = is_os->projs<5>();
    auto [r, s]                = callee->decurry()->args<2>();
    auto lr                    = Lit::isa(r);
    auto ls                    = Lit::isa(s);

    // TODO commute
    // TODO reassociate
    // TODO more than one Os
    // TODO select which Is/Os to zip

    if (lr && ls && *lr == 1 && *ls == 1) return w.app(f, arg);

    if (auto l_in = Lit::isa(n_i)) {
        auto args = arg->projs(*l_in);

        if (lr && std::ranges::all_of(args, [](const Def* arg) { return arg->isa<Prod>(); })) {
            auto shapes = s->projs(*lr);
            auto s_n    = Lit::isa(shapes.front());

            if (s_n) {
                auto elems = DefVec(*s_n, [&, f = f](size_t s_i) {
                    auto inner_args = DefVec(args.size(), [&](size_t i) { return args[i]->proj(*s_n, s_i); });
                    if (*lr == 1) {
                        return w.app(f, inner_args);
                    } else {
                        auto app_zip = w.app(w.annex<zip>(), {w.lit_nat(*lr - 1), w.tuple(shapes.view().subspan(1))});
                        return w.app(w.app(app_zip, is_os), inner_args);
                    }
                });
                return w.tuple(elems);
            }
        }
    }

    return {};
}

MIM_tuple_NORMALIZER_IMPL

} // namespace mim::plug::tuple
