#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>

#include <mim/def.h>
#include <mim/world.h>
#include <mim/tuple.h>

namespace nb = nanobind;
namespace mim {

static void ndarray_to_tuple_impl(World& w, const nb::ndarray<>& arr,
                                  std::vector<const Def*>& elems) {
    auto code      = arr.dtype().code;
    auto bits      = arr.dtype().bits;
    auto* data     = static_cast<const uint8_t*>(arr.data());
    auto ndim      = arr.ndim();
    auto itemsize  = arr.itemsize();

    if (static_cast<nb::dlpack::dtype_code>(code) != nb::dlpack::dtype_code::Int &&
        static_cast<nb::dlpack::dtype_code>(code) != nb::dlpack::dtype_code::UInt)
        throw nb::type_error(
            ("Only integer dtypes are supported (got code=" +
             std::to_string(static_cast<int>(code)) +
             ", bits=" + std::to_string(bits) + ")").c_str());

    size_t num_elems = 1;
    for (size_t i = 0; i < ndim; ++i)
        num_elems *= arr.shape(i);

    // DLPack strides are in elements, not bytes.
    // C-contiguous means innermost stride == 1.
    bool contiguous = ndim == 0;
    if (ndim > 0) {
        contiguous = true;
        int64_t expected = 1;
        for (int64_t i = static_cast<int64_t>(ndim) - 1; i >= 0; --i) {
            auto s = arr.stride(static_cast<size_t>(i));
            if (s != expected) { contiguous = false; break; }
            expected *= arr.shape(static_cast<size_t>(i));
        }
    }

    if (contiguous) {
        for (size_t i = 0; i < num_elems; ++i) {
            uint64_t val = 0;
            memcpy(&val, data + i * itemsize, itemsize);
            elems.push_back(static_cast<const Def*>(w.lit_int(bits, val)));
        }
    } else {
        std::vector<int64_t> idx(ndim, 0);
        for (size_t linear = 0; linear < num_elems; ++linear) {
            int64_t elem_offset = 0;
            for (size_t d = 0; d < ndim; ++d)
                elem_offset += idx[d] * arr.stride(d);
            uint64_t val = 0;
            memcpy(&val, data + elem_offset * itemsize, itemsize);
            elems.push_back(static_cast<const Def*>(w.lit_int(bits, val)));

            for (int64_t d = static_cast<int64_t>(ndim) - 1; d >= 0; --d) {
                auto dd = static_cast<size_t>(d);
                idx[dd]++;
                if (static_cast<int64_t>(idx[dd]) < static_cast<int64_t>(arr.shape(dd)))
                    break;
                idx[dd] = 0;
            }
        }
    }
}

void init_ndarray(nb::module_& m) {
    m.def("ndarray_to_tuple", [](World& w, const nb::ndarray<>& arr) {
        std::vector<const Def*> elems;
        ndarray_to_tuple_impl(w, arr, elems);
        return static_cast<Def*>(
            const_cast<Def*>(static_cast<const Def*>(w.tuple(elems))));
    }, nb::rv_policy::reference);
}

} // namespace mim
