#pragma once

#include <nanobind/nanobind.h>

#include "mim/axm.h"
#include "mim/check.h"
#include "mim/def.h"
#include "mim/lam.h"
#include "mim/lattice.h"
#include "mim/rule.h"
#include "mim/tuple.h"

namespace nanobind::detail {

/// Tells nanobind which Python class a `mim::Def*` should surface as.
///
/// nanobind normally recovers the most-derived type via `typeid(*ptr)`, but that only works for
/// *polymorphic* C++ types - and mim::Def deliberately has no vtable, since a vptr would cost 8 bytes on
/// every single node in the World (its former `virtual`s are dispatched on Def::node() instead; see the
/// dispatchers in def.cpp). Def carries its own type tag, so map Def::node() back to the matching
/// `std::type_info`. Without this hook every Def reaches Python as the base `Def` instead of as `App`,
/// `Lam`, ... - which is exactly what py/tests/nodes.py::test_every_node_constructs checks.
///
/// This is sound because Def is the *first* (and only non-empty) base of every node class, so a
/// `Def*` and the derived pointer share one address.
template<>
struct type_hook<mim::Def> {
    static const std::type_info* get(mim::Def* def) {
        if (!def) return &typeid(mim::Def);
        switch (def->node()) {
#define CODE(n, _) \
    case mim::Node::n: return &typeid(mim::n);
            MIM_NODE(CODE)
#undef CODE
            default: return &typeid(mim::Def);
        }
    }
};

} // namespace nanobind::detail
