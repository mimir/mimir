#include "mim/plug/core/core.h"

#include <mim/config.h>
#include <mim/pass.h>

#include "mim/plug/core/be/ll.h"
#include "mim/plug/core/be/sexpr.h"

using namespace mim;
using namespace mim::plug;

extern "C" MIM_EXPORT Plugin mim_get_plugin() {
    return {"core", MIM_VERSION, core::register_normalizers, nullptr, [](Backends& backends) {
                backends["ll"]            = &ll::emit;
                backends["sexpr"]         = &sexpr::emit;
                backends["sexpr-slotted"] = &sexpr::emit_slotted;
            }};
}

namespace mim::plug::core {

template<bool up>
const Sigma* convert(const TBound<up>* b) {
    auto& w = b->world();

    if constexpr (up) {
        nat_t align = 0;
        nat_t size  = 0;

        for (auto op : b->ops()) {
            auto a = Lit::isa(w.call(trait::align, op));
            auto s = Lit::isa(w.call(trait::size, op));
            if (!a || !s) return nullptr;

            align = std::max(align, *a);
            size  = std::max(size, *s);
        }

        assert(size % align == 0);
        auto arr = w.arr(size / align, w.type_int(align * 8_u64));

        return w.sigma({w.type_idx(b->num_ops()), arr})->template as<Sigma>();
    } else {
        return w.sigma(b->ops())->template as<Sigma>();
    }
}

#ifndef DOXYGEN
template const Sigma* convert(const TBound<false>*);
template const Sigma* convert(const TBound<true>*);
#endif

} // namespace mim::plug::core
