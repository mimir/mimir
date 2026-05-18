#include <Python.h>

#include <nanobind/nanobind.h>
// nb_type_isinstance is declared in nb_lib.h (included by nanobind.h)

#include <mim/def.h>
#include <mim/world.h>

namespace nb = nanobind;
namespace mim {

/// Return the smallest bit-width that can represent `val` as an unsigned value.
static nat_t min_uint_width(uint64_t val) {
    if (val < 256)                        return 8;
    if (val < 65536)                      return 16;
    if (val < 4294967296ULL)              return 32;
    return 64;
}

/// Return the smallest bit-width that can represent `val` as a signed value.
static nat_t min_sint_width(int64_t val) {
    if (val >= -128 && val <= 127)        return 8;
    if (val >= -32768 && val <= 32767)    return 16;
    if (val >= -2147483648LL && val <= 2147483647LL) return 32;
    return 64;
}



static nb::object py_to_mim(World& w, nb::handle obj) {
    // 1. Already a MimIR Def or Sym -> return as-is
    if (nb::detail::nb_type_isinstance(obj.ptr(), &typeid(Def)))
        return nb::borrow(obj);
    if (nb::detail::nb_type_isinstance(obj.ptr(), &typeid(fe::Sym)))
        return nb::borrow(obj);

    // 2. bool (must be checked before int, since bool is int subclass)
    if (PyBool_Check(obj.ptr())) {
        auto lit = static_cast<const Def*>(w.lit_bool(obj.ptr() == Py_True));
        return nb::cast(const_cast<Def*>(lit), nb::rv_policy::reference);
    }

    if (PyLong_Check(obj.ptr())) {
        int overflow;
        auto sval = PyLong_AsLongLongAndOverflow(obj.ptr(), &overflow);
        if (!overflow) {
            auto width = min_sint_width(static_cast<int64_t>(sval));
            auto uval = static_cast<uint64_t>(static_cast<int64_t>(sval));
            uint64_t mask = width < 64 ? ((uint64_t(1) << width) - 1) : ~uint64_t(0);
            auto lit = static_cast<const Def*>(w.lit_int(width, uval & mask));
            return nb::cast(const_cast<Def*>(lit), nb::rv_policy::reference);
        }
        PyErr_Clear();
        auto uval = PyLong_AsUnsignedLongLong(obj.ptr());
        if (uval == static_cast<uint64_t>(-1) && PyErr_Occurred()) {
            PyErr_Clear();
            throw nb::value_error("Integer value is too large for 64-bit");
        }
        auto width = min_uint_width(uval);
        uint64_t mask = width < 64 ? ((uint64_t(1) << width) - 1) : ~uint64_t(0);
        auto lit = static_cast<const Def*>(w.lit_int(width, uval & mask));
        return nb::cast(const_cast<Def*>(lit), nb::rv_policy::reference);
    }

    // 4. str -> Sym
    if (PyUnicode_Check(obj.ptr())) {
        Py_ssize_t size;
        const char* str = PyUnicode_AsUTF8AndSize(obj.ptr(), &size);
        auto sym = w.sym(std::string_view(str, static_cast<size_t>(size)));
        return nb::cast(sym);
    }

    // 5. list / tuple -> recursively cast to Tuple
    if (PyList_Check(obj.ptr()) || PyTuple_Check(obj.ptr())) {
        auto len = PySequence_Length(obj.ptr());
        if (len < 0)
            throw nb::value_error("Cannot determine sequence length");
        std::vector<const Def*> elems;
        elems.reserve(static_cast<size_t>(len));
        for (Py_ssize_t i = 0; i < len; ++i) {
            auto item = nb::steal(PySequence_GetItem(obj.ptr(), i));
            if (!item)
                throw nb::value_error("Failed to get sequence item");
            auto casted = py_to_mim(w, item);
            if (!nb::detail::nb_type_isinstance(casted.ptr(), &typeid(Def)))
                throw nb::type_error(
                    "All elements in a list/tuple must be convertible to Def");
            elems.push_back(nb::cast<const Def*>(casted));
        }
        auto tup = const_cast<Def*>(w.tuple(elems));
        return nb::cast(tup, nb::rv_policy::reference);
    }

    // 6. float — not yet supported
    if (PyFloat_Check(obj.ptr())) {
        throw nb::type_error(
            "Cannot cast float to MimIR type; float support requires the math plugin");
    }

    // 7. fallback
    auto type_name = Py_TYPE(obj.ptr())->tp_name;
    throw nb::type_error(
        ("Cannot cast Python type '" + std::string(type_name) +
         "' to MimIR type").c_str());
}

// ---------------------------------------------------------------------------
//  Module entry point
// ---------------------------------------------------------------------------

void init_cast(nb::module_& m) {
    m.def("cast", [](World& w, nb::handle obj) {
        return py_to_mim(w, obj);
    });
}

} // namespace mim
