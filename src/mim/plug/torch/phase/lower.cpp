#include "mim/plug/torch/phase/lower.h"

#include <string_view>

#include <fe/format.h>

#include <mim/axm.h>
#include <mim/check.h>
#include <mim/lam.h>

#include "mim/plug/runtime/runtime.h"
#include "mim/plug/torch/torch.h"

namespace mim::plug::torch::phase {

const Def* DecomposeByImpl::apply_impl(const App* app, flags_t impl_flags) {
    auto [axm, curry, remaining] = Axm::get(app);
    auto [_, args] = app->uncurry();
    (void)curry;
    (void)remaining;
    auto impl      = new_world().annex(impl_flags);
    if (!impl) fe::throwf("implementation annex `{}` is unavailable", impl_flags);

    try {
        for (auto arg : args)
            impl = new_world().app(impl, rewrite(arg));
        profile_count("decompose.impl");
        profile_count("decompose.impl." + axm->sym().str());
        return decompose_generated(impl);
    } catch (const std::exception& e) {
        fe::throwf("while decomposing `{}`: {}", axm->sym(), e.what());
    }
}

const Def* DecomposeByImpl::decompose_generated(const Def* def) {
    if (auto i = generated_.find(def); i != generated_.end()) return i->second;
    if (def->isa<Var>() || def->isa<Axm>()) return generated_[def] = def;

    if (auto mut = def->isa_mut()) {
        generated_[def] = def;
        if (!mut->is_set()) return def;
        auto new_type = decompose_generated(mut->type());
        auto new_ops  = DefVec(mut->num_ops(), [&](size_t i) { return decompose_generated(mut->op(i)); });
        bool changed  = new_type != mut->type();
        for (size_t i = 0; i != mut->num_ops(); ++i)
            changed |= new_ops[i] != mut->op(i);
        if (changed) {
            mut->unset();
            mut->set_type(new_type);
            mut->set(new_ops);
        }
        return def;
    }

    if (auto app = def->isa<App>()) {
        auto [axm, curry, remaining] = Axm::get(app);
        (void)remaining;
        if (axm && curry == 0)
            if (auto i = impls_.find(axm->flags()); i != impls_.end()) {
                if (!active_.emplace(app).second)
                    fe::throwf("cyclic implementation decomposition at `{}`", axm->sym());
                try {
                    auto [_, args] = app->uncurry();
                    auto impl      = new_world().annex(i->second);
                    if (!impl) fe::throwf("implementation annex `{}` is unavailable", i->second);
                    for (auto arg : args)
                        impl = new_world().app(impl, decompose_generated(arg));
                    profile_count("decompose.impl");
                    profile_count("decompose.impl." + axm->sym().str());
                    auto lowered = decompose_generated(impl);
                    active_.erase(app);
                    return generated_[def] = lowered;
                } catch (const std::exception& e) {
                    active_.erase(app);
                    fe::throwf("while decomposing `{}`: {}", axm->sym(), e.what());
                }
            }

        auto new_arg    = decompose_generated(app->arg());
        auto new_callee = decompose_generated(app->callee());
        if (auto pi = new_callee->type()->isa<Pi>())
            if (!Checker::assignable(pi->dom(), new_arg) && new_arg->num_projs() == pi->num_doms())
                new_arg = new_world().tuple(pi->dom(), DefVec(pi->num_doms(), [&](size_t i) {
                    return new_arg->proj(i);
                }));
        return generated_[def] = new_world().app(new_callee, new_arg);
    }

    auto new_type = def->type() ? decompose_generated(def->type()) : nullptr;
    auto new_ops  = DefVec(def->num_ops(), [&](size_t i) { return decompose_generated(def->op(i)); });
    bool changed  = new_type != def->type();
    for (size_t i = 0; i != def->num_ops(); ++i)
        changed |= new_ops[i] != def->op(i);
    if (!changed) return generated_[def] = def;

    Rewriter rebuild(new_world());
    if (def->type()) rebuild.map(def->type(), new_type);
    for (size_t i = 0; i != def->num_ops(); ++i)
        rebuild.map(def->op(i), new_ops[i]);
    return generated_[def] = rebuild.rewrite(def);
}

const Def* DecomposeByImpl::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto [axm, curry, remaining] = Axm::get(app);
    (void)remaining;
    if (!axm || curry != 0) return RWPhase::rewrite_imm_App(app);
    auto i = impls_.find(axm->flags());
    if (i == impls_.end()) return RWPhase::rewrite_imm_App(app);
    if (!active_.emplace(app).second)
        fe::throwf("cyclic implementation decomposition at `{}`", axm->sym());
    auto lowered = apply_impl(app, i->second);
    active_.erase(app);
    return lowered;
}

void DecomposeByImpl::verify_closed(const Def* root) {
    DefSet seen;
    auto visit = [&](auto&& self, const Def* def) -> void {
        if (!seen.emplace(def).second) return;
        if (auto [axm, curry, remaining] = Axm::get(def); axm && curry == 0) {
            (void)remaining;
            if (impls_.contains(axm->flags()))
                fe::throwf("implementation decomposition left residual operator `{}`", axm->sym());
            if (auto i = residual_counters_.find(axm->base());
                i != residual_counters_.end() && observed_.emplace(def).second)
                profile_count(i->second);
        }
        for (auto op : def->deps())
            self(self, op);
    };
    visit(visit, root);
}

void DecomposeByImpl::rewrite_external(Def* old_mut) {
    RWPhase::rewrite_external(old_mut);
    auto new_mut = lookup(old_mut);
    assert(new_mut);
    verify_closed(new_mut);
}

namespace {

std::string normalize_op_name(std::string_view name) {
    if (name.starts_with("%torch.")) name.remove_prefix(7);
    else if (name.starts_with("torch.")) name.remove_prefix(6);
    return std::string(name);
}

} // namespace

Lower::Lower(World& world, flags_t annex)
    : Lower(world, annex, false) {}

Decompose::Decompose(World& world, flags_t annex)
    : Lower(world, annex, true) {}

Lower::Lower(World& world, flags_t annex, bool selective)
    : DecomposeByImpl(world, annex) {
    if (selective) {
        for (const auto& arg : args()) {
            constexpr std::string_view prefix = "preserve=";
            std::string_view value(arg);
            if (!value.starts_with(prefix))
                fe::throwf("unknown torch phase argument `{}`; expected `preserve=<op>[,<op>...]`", arg);
            value.remove_prefix(prefix.size());
            if (value.empty()) fe::throwf("empty operator in torch phase argument `{}`", arg);
            while (true) {
                auto comma = value.find(',');
                auto name  = value.substr(0, comma);
                if (name.empty()) fe::throwf("empty operator in torch phase argument `{}`", arg);
                preserved_.insert(normalize_op_name(name));
                if (comma == std::string_view::npos) break;
                value.remove_prefix(comma + 1);
                if (value.empty()) fe::throwf("empty operator in torch phase argument `{}`", arg);
            }
        }
    }

    observe<runtime::static_check>("constraints.dynamic_residual");
#define MIM_BIND_TORCH_IMPL(src) bind_if_enabled<torch::src, torch::src##_impl>(#src)
#define MIM_BIND_TORCH_SUB(family, src, legacy)                                      \
    bind_sub_if_enabled<torch::family::src, torch::family##_impl::src>(              \
        #family "." #src, #legacy)
    // Pointwise arithmetic, comparisons, and activations.
    MIM_BIND_TORCH_SUB(binary, add, add_op);
    MIM_BIND_TORCH_SUB(binary, sub, sub_op);
    MIM_BIND_TORCH_SUB(binary, mul, mul_op);
    MIM_BIND_TORCH_SUB(binary, div, div_op);
    MIM_BIND_TORCH_SUB(binary, add_scalar, add_scalar_op);
    MIM_BIND_TORCH_SUB(binary, add_i64_scalar, add_i64_scalar_op);
    MIM_BIND_TORCH_SUB(binary, sub_scalar, sub_scalar_op);
    MIM_BIND_TORCH_SUB(binary, add_scalar_lhs, add_scalar_lhs_op);
    MIM_BIND_TORCH_SUB(binary, sub_scalar_lhs, sub_scalar_lhs_op);
    MIM_BIND_TORCH_SUB(binary, sub_i64_scalar, sub_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, ne_i64_scalar, ne_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, eq_i64_scalar, eq_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, lt_i64_scalar, lt_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, le_i64_scalar, le_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, gt_i64_scalar, gt_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, ge_i64_scalar, ge_i64_scalar_op);
    MIM_BIND_TORCH_SUB(comparison, eq_i64, eq_i64_op);
    MIM_BIND_TORCH_SUB(comparison, ne_i64, ne_i64_op);
    MIM_BIND_TORCH_SUB(comparison, lt_i64, lt_i64_op);
    MIM_BIND_TORCH_SUB(comparison, le_i64, le_i64_op);
    MIM_BIND_TORCH_SUB(comparison, gt_i64, gt_i64_op);
    MIM_BIND_TORCH_SUB(comparison, ge_i64, ge_i64_op);
    MIM_BIND_TORCH_SUB(binary, mul_scalar, mul_scalar_op);
    MIM_BIND_TORCH_SUB(binary, pow_tensor_scalar, pow_tensor_scalar_op);
    MIM_BIND_TORCH_SUB(binary, addcmul, addcmul_op);
    MIM_BIND_TORCH_SUB(binary, addcdiv, addcdiv_op);
    MIM_BIND_TORCH_SUB(binary, maximum, maximum_op);
    MIM_BIND_TORCH_SUB(binary, minimum, minimum_op);
    MIM_BIND_TORCH_SUB(comparison, eq, eq_op);
    MIM_BIND_TORCH_SUB(comparison, ne, ne_op);
    MIM_BIND_TORCH_SUB(comparison, lt, lt_op);
    MIM_BIND_TORCH_SUB(comparison, le, le_op);
    MIM_BIND_TORCH_SUB(comparison, gt, gt_op);
    MIM_BIND_TORCH_SUB(comparison, ge, ge_op);
    bind_sub_if_enabled<torch::activation::relu, torch::activation_impl::relu>(
        "activation.relu", "relu_op");
    MIM_BIND_TORCH_SUB(activation, leaky_relu, leaky_relu_op);
    MIM_BIND_TORCH_SUB(unary, neg, neg_op);
    MIM_BIND_TORCH_SUB(unary, abs, abs_op);
    MIM_BIND_TORCH_SUB(unary, exp, exp_op);
    MIM_BIND_TORCH_SUB(unary, log, log_op);
    bind_sub_if_enabled<torch::activation::tanh, torch::activation_impl::tanh>(
        "activation.tanh", "tanh_op");
    MIM_BIND_TORCH_SUB(unary, sqrt, sqrt_op);
    MIM_BIND_TORCH_SUB(unary, rsqrt, rsqrt_op);
    MIM_BIND_TORCH_SUB(unary, sin, sin_op);
    MIM_BIND_TORCH_SUB(unary, cos, cos_op);
    MIM_BIND_TORCH_SUB(unary, reciprocal, reciprocal_op);
    bind_sub_if_enabled<torch::activation::sigmoid, torch::activation_impl::sigmoid>(
        "activation.sigmoid", "sigmoid_op");
    bind_sub_if_enabled<torch::activation::silu, torch::activation_impl::silu>(
        "activation.silu", "silu_op");
    bind_sub_if_enabled<torch::activation::gelu, torch::activation_impl::gelu>(
        "activation.gelu", "gelu_op");
    MIM_BIND_TORCH_SUB(activation, elu, elu_op);
    MIM_BIND_TORCH_SUB(activation, selu, selu_op);
    MIM_BIND_TORCH_SUB(activation, hardsigmoid, hardsigmoid_op);
    MIM_BIND_TORCH_SUB(activation, softplus, softplus_op);
    MIM_BIND_TORCH_SUB(activation, mish, mish_op);
    bind_sub_if_enabled<torch::activation::hardtanh, torch::activation_impl::hardtanh>(
        "activation.hardtanh", "hardtanh_op");
    MIM_BIND_TORCH_SUB(activation, threshold, threshold_op);
    MIM_BIND_TORCH_SUB(activation, clamp, clamp_op);
    MIM_BIND_TORCH_SUB(pointwise, where_, where_op);
    MIM_BIND_TORCH_SUB(pointwise, masked_fill_scalar, masked_fill_scalar_op);

    // Shape, view, indexing, and construction operators.
    MIM_BIND_TORCH_SUB(shape, reshape, reshape_op);
    MIM_BIND_TORCH_SUB(shape, permute, permute_op);
    MIM_BIND_TORCH_SUB(shape, transpose_int, transpose_int_op);
    MIM_BIND_TORCH_SUB(shape, expand, expand_op);
    MIM_BIND_TORCH_SUB(indexing, slice, slice_op);
    MIM_BIND_TORCH_SUB(indexing, select, select_op);
    MIM_BIND_TORCH_SUB(shape, cat, cat_op);
    MIM_BIND_TORCH_SUB(shape, flatten, flatten_op);
    MIM_BIND_TORCH_SUB(shape, squeeze, squeeze_op);
    MIM_BIND_TORCH_SUB(shape, unsqueeze, unsqueeze_op);
    MIM_BIND_TORCH_SUB(shape, repeat, repeat_op);
    MIM_BIND_TORCH_SUB(indexing, flip, flip_op);
    MIM_BIND_TORCH_SUB(indexing, roll, roll_op);
    MIM_BIND_TORCH_SUB(indexing, unfold, unfold_op);
    MIM_BIND_TORCH_SUB(indexing, narrow, narrow_op);
    MIM_BIND_TORCH_SUB(creation, constant_pad, constant_pad_op);
    MIM_BIND_TORCH_SUB(creation, full, full_op);
    MIM_BIND_TORCH_SUB(creation, scalar_tensor, scalar_tensor_op);
    MIM_BIND_TORCH_SUB(creation, full_like, full_like_op);
    MIM_BIND_TORCH_SUB(creation, empty_strided, empty_strided_op);
    MIM_BIND_TORCH_SUB(creation, fill_scalar, fill_scalar_op);
    MIM_BIND_TORCH_SUB(creation, arange_i64, arange_i64_op);
    MIM_BIND_TORCH_SUB(creation, clone, clone_op);
    MIM_BIND_TORCH_SUB(creation, copy, copy_op);
    MIM_BIND_TORCH_SUB(creation, lift_fresh_copy, lift_fresh_copy_op);

    // Reductions.
    MIM_BIND_TORCH_SUB(reduction, sum_dim, sum_dim_op);
    MIM_BIND_TORCH_SUB(reduction, sum_dim_keepdim, sum_dim_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, amax_dim, amax_dim_op);
    MIM_BIND_TORCH_SUB(reduction, max_dim, max_dim_op);
    MIM_BIND_TORCH_SUB(reduction, min_dim, min_dim_op);
    MIM_BIND_TORCH_SUB(reduction, mean_dim, mean_dim_op);
    MIM_BIND_TORCH_SUB(reduction, mean_dim_keepdim, mean_dim_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, sum_dims, sum_dims_op);
    MIM_BIND_TORCH_SUB(reduction, sum_dims_keepdim, sum_dims_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, norm2_dims, norm2_dims_op);
    MIM_BIND_TORCH_SUB(reduction, norm2_dims_keepdim, norm2_dims_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, norm2_all, norm2_all_op);
    MIM_BIND_TORCH_SUB(reduction, sum_all, sum_all_op);
    MIM_BIND_TORCH_SUB(reduction, mean_all, mean_all_op);
    MIM_BIND_TORCH_SUB(loss, smooth_l1_mean, smooth_l1_mean_op);
    MIM_BIND_TORCH_SUB(loss, kl_div_reduced, kl_div_reduced_op);
    MIM_BIND_TORCH_SUB(loss, triplet_margin_reduced, triplet_margin_reduced_op);
    MIM_BIND_TORCH_SUB(reduction, amax_dims, amax_dims_op);
    MIM_BIND_TORCH_SUB(reduction, mean_dims, mean_dims_op);
    MIM_BIND_TORCH_SUB(reduction, mean_dims_keepdim, mean_dims_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, logsumexp_dims, logsumexp_dims_op);
    MIM_BIND_TORCH_SUB(reduction, logsumexp_dims_keepdim, logsumexp_dims_keepdim_op);
    MIM_BIND_TORCH_SUB(reduction, var_mean_dims, var_mean_dims_op);
    MIM_BIND_TORCH_SUB(reduction, any_dims, any_dims_op);
    MIM_BIND_TORCH_SUB(reduction, all_dims, all_dims_op);
    MIM_BIND_TORCH_SUB(scan, cumsum_2d, cumsum_2d_op);
    MIM_BIND_TORCH_SUB(scan, cumsum_2d_direction, cumsum_2d_direction_op);
    MIM_BIND_TORCH_SUB(scan, cumsum_exclusive_2d, cumsum_exclusive_2d_op);
    MIM_BIND_TORCH_SUB(scan, cumsum_bool_i64, cumsum_bool_i64_op);
    MIM_BIND_TORCH_SUB(scan, cumprod_2d, cumprod_2d_op);

    // Linear algebra and neural-network operators.
    MIM_BIND_TORCH_SUB(linalg, mm, mm_op);
    MIM_BIND_TORCH_SUB(linalg, bmm, bmm_op);
    MIM_BIND_TORCH_SUB(linalg, matmul, matmul_op);
    bind_sub_if_enabled<torch::conv::general, torch::conv_impl::general>(
        "conv.general", "convolution_op");
    bind_sub_if_enabled<torch::conv::conv1d, torch::conv_impl::conv1d>(
        "conv.conv1d", "convolution1d_op");
    bind_sub_if_enabled<torch::conv::conv3d, torch::conv_impl::conv3d>(
        "conv.conv3d", "convolution3d_op");
    bind_sub_if_enabled<torch::conv::transpose2d, torch::conv_impl::transpose2d>(
        "conv.transpose2d", "convolution_transpose2d_op");
    bind_sub_if_enabled<torch::conv::transpose1d, torch::conv_impl::transpose1d>(
        "conv.transpose1d", "convolution_transpose1d_op");
    bind_sub_if_enabled<torch::conv::transpose3d, torch::conv_impl::transpose3d>(
        "conv.transpose3d", "convolution_transpose3d_op");
    MIM_BIND_TORCH_SUB(pool, max_pool1d, max_pool1d_op);
    MIM_BIND_TORCH_SUB(pool, max_pool2d, max_pool2d_op);
    MIM_BIND_TORCH_SUB(pool, max_pool2d_with_indices, max_pool2d_with_indices_op);
    MIM_BIND_TORCH_SUB(pool, max_pool3d, max_pool3d_op);
    MIM_BIND_TORCH_SUB(pool, avg_pool1d, avg_pool1d_op);
    MIM_BIND_TORCH_SUB(pool, avg_pool2d, avg_pool2d_op);
    MIM_BIND_TORCH_SUB(pool, avg_pool3d, avg_pool3d_op);
    MIM_BIND_TORCH_SUB(pool, adaptive_avg_pool1d, adaptive_avg_pool1d_op);
    MIM_BIND_TORCH_SUB(pool, adaptive_avg_pool3d, adaptive_avg_pool3d_op);
    MIM_BIND_TORCH_SUB(linalg, addmm, addmm_op);
    MIM_BIND_TORCH_SUB(linalg, linear, linear_op);
    MIM_BIND_TORCH_SUB(indexing, gather, gather_op);
    MIM_BIND_TORCH_SUB(indexing, scatter_src, scatter_src_op);
    MIM_BIND_TORCH_SUB(indexing, scatter_value, scatter_value_op);
    MIM_BIND_TORCH_SUB(indexing, embedding, embedding_op);
    MIM_BIND_TORCH_SUB(indexing, index_2d, index_2d_op);
    MIM_BIND_TORCH_SUB(normalization, native_layer_norm, native_layer_norm_op);
    MIM_BIND_TORCH_SUB(normalization, native_group_norm, native_group_norm_op);
    MIM_BIND_TORCH_SUB(normalization, group_norm, group_norm_op);
    MIM_BIND_TORCH_SUB(normalization, batch_norm_inference, batch_norm_inference_op);
    MIM_BIND_TORCH_SUB(normalization, softmax, softmax_op);
    MIM_BIND_TORCH_SUB(normalization, log_softmax, log_softmax_op);
    MIM_BIND_TORCH_SUB(linalg, triu, triu_op);
    MIM_BIND_TORCH_SUB(linalg, tril, tril_op);

    // Metadata-only framework assertions.
    MIM_BIND_TORCH_SUB(metadata, assert_tensor_metadata, assert_tensor_metadata_op);
#undef MIM_BIND_TORCH_IMPL
#undef MIM_BIND_TORCH_SUB

    if (!preserved_.empty())
        fe::throwf("unknown Torch operator in `preserve`: `{}`", *preserved_.begin());
}

} // namespace mim::plug::torch::phase
