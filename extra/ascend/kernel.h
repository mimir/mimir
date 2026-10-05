#pragma once

#include <cstdint>

#include <string>

#include <mim/world.h>

namespace mim::plug::ascend {

enum class BinOp { add, sub, mul, div };

/// A MimIR kernel reduced to the subset both Ascend emitters accept:
/// a DAG of same-shape elementwise binaries over F32 tensor parameters.
/// A value is addressed by an id: the first Kernel::num_params ids are the parameters, the rest index Kernel::nodes.
struct Kernel {
    struct Node {
        BinOp op;
        size_t lhs, rhs;
    };

    std::string name;
    fe::Vector<int64_t> shape;
    size_t num_params = 0;
    fe::Vector<Node> nodes;
    size_t result = 0;

    size_t num_values() const { return num_params + nodes.size(); }
    bool is_param(size_t id) const { return id < num_params; }
    const Node& node(size_t id) const { return nodes[id - num_params]; }
    size_t node_id(size_t index) const { return num_params + index; }

    /// Number of tensor elements, which is also the number of elements of every intermediate buffer.
    uint64_t num_elems() const;
};

/// Reduces the World's single external function to a Kernel, or throws with a diagnostic.
Kernel analyze(World&);

} // namespace mim::plug::ascend
