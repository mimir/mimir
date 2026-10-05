#include "kernel.h"

#include <cctype>

#include <limits>
#include <optional>

#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/math/math.h>
#include <mim/plug/tensor/tensor.h>

namespace mim::plug::ascend {

namespace {

Lam* entry_of(World& world) {
    Lam* entry = nullptr;
    for (auto external : world.externals().muts()) {
        auto lam = external->isa_mut<Lam>();
        if (!lam) fe::throwf("ascend backend expected an external function, but got `{}`", external);
        if (entry) fe::throwf("ascend backend supports exactly one external function");
        entry = lam;
    }

    if (!entry) fe::throwf("ascend backend requires one external function");
    if (!entry->is_set() || Pi::isa_cn(entry->type()))
        fe::throwf("ascend backend requires a set direct-style external function, but got `{}`", entry);
    return entry;
}

std::string symbol_of(Lam* lam) {
    auto name = lam->sym().view();
    if (name.empty()) fe::throwf("ascend backend requires a named external function");

    for (auto c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' || c == '.'))
            fe::throwf("ascend backend cannot emit function name `{}`", name);
    return std::string(name);
}

fe::Vector<int64_t> shape_of(const Def* type) {
    auto shape = fe::Vector<int64_t>();
    while (auto arr = type->isa<Arr>()) {
        auto extent = Lit::isa<nat_t>(arr->arity());
        if (!extent) fe::throwf("ascend backend requires static tensor extents, but got `{}`", arr->arity());
        if (*extent > static_cast<nat_t>(std::numeric_limits<int64_t>::max()))
            fe::throwf("ascend backend tensor extent `{}` is too large", *extent);
        shape.emplace_back(static_cast<int64_t>(*extent));
        type = arr->body();
    }

    if (shape.empty()) fe::throwf("ascend backend expected a tensor type, but got `{}`", type);
    if (!math::match_f32(type)) fe::throwf("ascend backend only supports F32 tensor elements, but got `{}`", type);
    return shape;
}

/// `tensor.binary` curries element types, the scalar function, the shape, and the operands; only the function matters.
std::optional<BinOp> match_arith(const Def* def) {
    auto op = std::optional<BinOp>();
    if (Axm::isa<math::arith, 1>(math::arith::add, def))
        op = BinOp::add;
    else if (Axm::isa<math::arith, 1>(math::arith::sub, def))
        op = BinOp::sub;
    else if (Axm::isa<math::arith, 1>(math::arith::mul, def))
        op = BinOp::mul;
    else if (Axm::isa<math::arith, 1>(math::arith::div, def))
        op = BinOp::div;
    if (!op) return {};

    auto args = App::uncurry_args<2>(def);
    if (Lit::isa(args[0]) != 0 || !math::match_f32(def->type()->as<Pi>()->codom())) return {};
    return op;
}

class Analyzer {
public:
    explicit Analyzer(World& world)
        : entry_(entry_of(world)) {}

    Kernel run() {
        kernel_.name  = symbol_of(entry_);
        kernel_.shape = shape_of(entry_->codom());

        auto n = entry_->num_vars();
        if (n == 0) fe::throwf("ascend backend requires at least one tensor parameter");
        for (size_t i = 0; i != n; ++i) {
            auto var = entry_->var(n, i);
            require_shape(var->type());
            ids_.emplace(var, i);
        }

        kernel_.num_params = n;
        kernel_.result     = lower(entry_->body());
        return std::move(kernel_);
    }

private:
    void require_shape(const Def* type) const {
        if (shape_of(type) != kernel_.shape) fe::throwf("ascend backend requires equal input and output tensor types");
    }

    size_t lower(const Def* def) {
        if (auto i = ids_.find(def); i != ids_.end()) return i->second;

        auto binary = Axm::isa<tensor::binary>(def);
        if (!binary) fe::throwf("ascend backend does not support value `{}`", def);

        auto operands = binary->uncurry_args<4>();
        auto op       = match_arith(operands[1]);
        if (!op)
            fe::throwf(
                "ascend backend only supports tensor.binary of F32 math.arith add, sub, mul, or div, but got `{}`",
                operands[1]);

        require_shape(def->type());
        auto [lhs, rhs] = operands[3]->projs<2>();
        require_shape(lhs->type());
        require_shape(rhs->type());

        auto node = Kernel::Node{*op, lower(lhs), lower(rhs)};
        auto id   = kernel_.node_id(kernel_.nodes.size());
        kernel_.nodes.emplace_back(node);
        ids_.emplace(def, id);
        return id;
    }

    Lam* entry_;
    Kernel kernel_;
    DefMap<size_t> ids_;
};

} // namespace

uint64_t Kernel::num_elems() const {
    auto elems = uint64_t{1};
    for (auto dim : shape) {
        auto extent = static_cast<uint64_t>(dim);
        if (extent != 0 && elems > std::numeric_limits<uint64_t>::max() / extent)
            fe::throwf("ascend backend tensor has too many elements");
        elems *= extent;
    }
    return elems;
}

Kernel analyze(World& world) { return Analyzer(world).run(); }

} // namespace mim::plug::ascend
