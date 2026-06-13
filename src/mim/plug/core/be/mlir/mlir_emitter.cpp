#include "mim/plug/core/be/mlir/mlir_emitter.h"

#include <format>
#include <functional>
#include <map>
#include <set>

#include <mim/lam.h>
#include <mim/tuple.h>

#include <mim/plug/affine/affine.h>
#include <mim/plug/affine/autogen.h>
#include <mim/plug/core/core.h>
#include <mim/plug/math/math.h>
#include <mim/plug/mem/mem.h>
#include <mim/plug/tensor/autogen.h>
#include <mim/plug/tensor/tensor.h>

namespace mim::mlir_be {

// ----- Helpers -------
std::string MLIREmitter::fresh_name(const Def* def) {
    auto sym = def->sym().str();
    if (!sym.empty() && sym[0] != '_') return "%" + sym;
    return std::format("%v{}", name_counter_++);
}
std::string MLIREmitter::fresh_name(std::string prefix) { return prefix + std::to_string(name_counter_++); }

bool MLIREmitter::is_return_callee(const Def* c, const Def* ret_var) {
    if (c == ret_var) return true;
    if (auto ex = c->isa<Extract>()) return ex->sym().str() == "return";
    return false;
}

MLIRValue MLIREmitter::get_or_emit(const Def* def, MLIRBlock& into) {
    if (auto it = values_.find(def); it != values_.end()) return it->second;
    auto val     = emit_def(def, into);
    values_[def] = val;
    return val;
}

// Recursively unpack op into individual MLIR argsv
void MLIREmitter::seed_dom_op(const Def* op, std::vector<MLIRValue>& args) {
    if (Axm::isa<plug::mem::M>(op->type())) return;
    if (op->type()->isa<Pi>()) return;

    if (auto sigma = op->type()->isa<Sigma>()) {
        for (size_t i = 0; i < sigma->num_ops(); ++i)
            seed_dom_op(op->proj(sigma->num_ops(), i), args);
        return;
    }

    MLIRValue v{fresh_name(op), types_.convert(op->type())};
    args.push_back(v);
    values_[op] = v;
}

// Recursively walks the var tree and seeds any unseeded leaf by sym match.
void MLIREmitter::seed_var_tree(const Def* d) {
    if (values_.contains(d)) return;
    if (Axm::isa<plug::mem::M>(d->type())) return;
    if (d->type()->isa<Pi>()) return;

    if (auto sigma = d->type()->isa<Sigma>()) {
        for (size_t i = 0; i < sigma->num_ops(); ++i)
            seed_var_tree(d->proj(sigma->num_ops(), i));
    } else if (auto arr = d->type()->isa<Arr>()) {
        if (auto n = Lit::isa(arr->arity()))
            for (size_t i = 0; i < *n; ++i)
                seed_var_tree(d->proj(*n, i));
    }

    if (!values_.contains(d)) {
        auto sym = d->sym().str();
        if (!sym.empty()) {
            for (auto& [seeded, v] : values_)
                if (v.name == "%" + sym) {
                    values_[d] = v;
                    return;
                }
        }
    }
}
// ----- main functions -----
//

void MLIREmitter::run() {
    clf_.run();

    MLIRRegion module;
    for (auto& [sym, def] : world_.externals())
        if (auto lam = def->isa_mut<Lam>())
            if (clf_.kind_of(lam) == LamKind::Function) emit_func(lam, module.entry());

    os_ << "module {\n";
    Printer p{os_};
    p.indent();
    p.print_region(module);
    p.dedent();
    os_ << "}\n";
}

// ---- emitters ----

void MLIREmitter::emit_func(Lam* lam, MLIRBlock& into) {
    values_.clear();
    name_counter_ = 0;
    curr_ret_var_ = lam->ret_var();

    std::vector<MLIRValue> args;
    auto dom = lam->type()->dom();
    if (auto sigma = dom->isa<Sigma>())
        for (size_t i = 0; i < sigma->num_ops(); ++i)
            seed_dom_op(lam->var()->proj(sigma->num_ops(), i), args);
    else
        seed_dom_op(lam->var(), args);

    std::vector<MLIRType> ret_types;
    if (auto ret_pi = lam->type()->ret_pi()) {
        auto ret_dom = ret_pi->dom();
        if (auto sigma = ret_dom->isa<Sigma>()) {
            for (auto op : sigma->ops()) {
                if (Axm::isa<plug::mem::M>(op)) continue;
                ret_types.push_back(types_.convert(op));
            }
        } else if (!Axm::isa<plug::mem::M>(ret_dom)) {
            ret_types.push_back(types_.convert(ret_dom));
        }
    }

    auto* func_op = new FuncOp(lam->sym().str(), args, ret_types, lam->is_external());
    emit_body(lam, func_op->body().entry());
    into.ops.emplace_back(func_op);
}

void MLIREmitter::emit_body(Lam* lam, MLIRBlock& into) {
    assert(lam->is_set() && "lam body not set");
    auto* app = lam->body()->isa<App>();
    assert(app && "expected App at lam body");
    auto* callee = app->callee();
    auto* arg    = app->arg();

    // func.return
    if (is_return_callee(callee, curr_ret_var_)) {
        std::vector<MLIRValue> ret_vals;
        if (auto sigma = arg->type()->isa<Sigma>()) {
            for (size_t i = 0; i < sigma->num_ops(); ++i) {
                if (Axm::isa<plug::mem::M>(sigma->op(i))) continue;
                auto v = get_or_emit(arg->proj(sigma->num_ops(), i), into);
                if (!v.empty()) ret_vals.push_back(v);
            }
        } else if (arg->type()->isa<Arr>()) {
            auto v = get_or_emit(arg, into);
            if (!v.empty()) ret_vals.push_back(v);
        } else if (!Axm::isa<plug::mem::M>(arg->type())) {
            auto v = get_or_emit(arg, into);
            if (!v.empty()) ret_vals.push_back(v);
        }
        into.ops.emplace_back(std::make_unique<FuncReturnOp>(std::move(ret_vals)));
        return;
    }

    if (Axm::isa<plug::affine::For>(app)) {
        emit_affine_for(app, into);
        return;
    }

    std::cerr << "unhandled callee: " << callee->node_name() << " sym='" << callee->sym().str() << "'\n";
    assert(false && "unhandled callee in emit_body");
}

void MLIREmitter::emit_affine_for(const App* app, MLIRBlock& into) {
    auto for_ax = Axm::isa<plug::affine::For>(app);
    assert(for_ax);

    auto [body_def, exit_def, loop_args] = for_ax->uncurry_args<3>();
    auto* body_lam                       = body_def->isa_mut<Lam>();
    auto* exit_lam                       = exit_def->isa_mut<Lam>();
    assert(body_lam && exit_lam);

    // collect iv and iter_args from body_lam domain
    MLIRValue iv_val;
    std::vector<MLIRValue> acc_bb;

    auto collect_body_var = [&](const Def* op_type, const Def* var_op) {
        if (Axm::isa<plug::mem::M>(op_type)) return;
        if (op_type->isa<Pi>()) return;
        auto sym = var_op->sym().str();
        MLIRValue v{"%" + (sym.empty() ? std::format("v{}", name_counter_++) : sym), types_.convert(op_type)};
        values_[var_op] = v;
        if (iv_val.empty())
            iv_val = v;
        else
            acc_bb.push_back(v);
    };

    if (auto sigma = body_lam->type()->dom()->isa<Sigma>())
        for (size_t i = 0; i < sigma->num_ops(); ++i)
            collect_body_var(sigma->op(i), body_lam->var()->proj(sigma->num_ops(), i));
    else
        collect_body_var(body_lam->type()->dom(), body_lam->var());

    // collect result values from exit_lam domain
    std::vector<MLIRValue> result_vals;

    auto collect_exit_var = [&](const Def* op_type, const Def* var_op) {
        if (Axm::isa<plug::mem::M>(op_type)) return;
        MLIRValue res{"%" + std::format("for_res{}", name_counter_++), types_.convert(op_type)};
        values_[var_op] = res;
        result_vals.push_back(res);
    };

    if (auto sigma = exit_lam->type()->dom()->isa<Sigma>())
        for (size_t i = 0; i < sigma->num_ops(); ++i)
            collect_exit_var(sigma->op(i), exit_lam->var()->proj(sigma->num_ops(), i));
    else
        collect_exit_var(exit_lam->type()->dom(), exit_lam->var());

    // emit lb, ub, step, init
    size_t n_total = 3 + acc_bb.size();
    auto lb_val    = get_or_emit(loop_args->proj(n_total, 0), into);
    auto ub_val    = get_or_emit(loop_args->proj(n_total, 1), into);
    auto step_val  = get_or_emit(loop_args->proj(n_total, 2), into);

    auto lb_idx   = types_.to_index(lb_val, into, fresh_name("%lb.idx"));
    auto ub_idx   = types_.to_index(ub_val, into, fresh_name("%ub.idx"));
    auto step_idx = types_.to_index(step_val, into, fresh_name("%step.idx"));

    // iv is always index in scf.for
    MLIRType orig_iv_type = iv_val.type;
    MLIRValue iv_idx{iv_val.name + ".idx", MLIRType{MLIRIndexType{}}};

    std::vector<MLIRValue> init_vals;
    for (size_t i = 0; i < acc_bb.size(); ++i)
        init_vals.push_back(get_or_emit(loop_args->proj(n_total, 3 + i), into));

    auto* for_op = new SCFForOp(result_vals, iv_idx, lb_idx, ub_idx, step_idx, acc_bb, init_vals);

    // cast iv back to original type at the start of the body if needed
    auto& body_bb = for_op->body().entry();
    if (!std::holds_alternative<MLIRIndexType>(orig_iv_type)) {
        MLIRValue iv_int{iv_val.name, orig_iv_type};
        body_bb.ops.emplace_back(std::make_unique<IndexCastOp>(iv_int, iv_idx));
        // re-seed all vars that were mapped to iv_val with the cast value
        for (auto& [d, v] : values_)
            if (v.name == iv_val.name) v = iv_int;
    }

    emit_for_body(body_lam, body_bb);
    into.ops.emplace_back(for_op);
    emit_body(exit_lam, into);
}

void MLIREmitter::emit_for_body(Lam* body_lam, MLIRBlock& body_bb) {
    assert(body_lam->is_set());
    auto* app = body_lam->body()->isa<App>();
    assert(app && "expected App in for body");
    auto* callee = app->callee();

    if (is_return_callee(callee, body_lam->ret_var())) {
        std::vector<MLIRValue> yield_vals;

        auto* arg = app->arg();
        if (auto sigma = arg->type()->isa<Sigma>()) {
            for (size_t i = 0; i < sigma->num_ops(); ++i) {
                if (Axm::isa<plug::mem::M>(sigma->op(i))) continue;
                auto v = get_or_emit(arg->proj(sigma->num_ops(), i), body_bb);
                if (!v.empty()) yield_vals.push_back(v);
            }
        } else if (!Axm::isa<plug::mem::M>(arg->type())) {
            auto v = get_or_emit(arg, body_bb);
            if (!v.empty()) yield_vals.push_back(v);
        }
        body_bb.ops.emplace_back(std::make_unique<SCFYieldOp>(std::move(yield_vals)));
        return;
    }

    assert(false && "unhandled callee in emit_for_body");
}

//  ---------- linalg -----------

void MLIREmitter::emit_linalg_generic(const App* app, MLIRBlock& into) {
    // Unpack currying chain:
    //   app0->arg = inputs pack
    //   app1->arg = subscripts
    //   app2->arg = (comb, zero)
    //   app3->arg = (Tis, Ris, Sis)   [skipped]
    //   app4->arg = S (result shape)
    //   app5->arg = (T, rank)
    auto* app0 = app;
    auto* app1 = app0->callee()->as<App>();
    auto* app2 = app1->callee()->as<App>();
    auto* app4 = app2->callee()->as<App>()->callee()->as<App>(); // skip app3
    auto* app5 = app4->callee()->as<App>();
    auto* app6 = app5->callee()->as<App>();

    auto* inputs_pack = app0->arg();
    auto* subs        = app1->arg();
    auto [comb, zero] = app2->arg()->projs<2>();
    auto* S           = app4->arg();
    auto [T, rank]    = app5->arg()->projs<2>();
    auto* nis         = app6->arg();

    auto n_inputs_opt = Lit::isa(nis);
    assert(n_inputs_opt && "nis must be a literal");
    size_t n_inputs = *n_inputs_opt;

    auto proj_input
        = [&](size_t i) -> const Def* { return n_inputs == 1 ? inputs_pack : inputs_pack->proj(n_inputs, i); };
    auto proj_sub = [&](size_t i) -> const Def* { return n_inputs == 1 ? subs : subs->proj(n_inputs, i); };

    auto* body_lam = comb->isa_mut<Lam>();
    assert(body_lam && "comb must be a mutable Lam");

    auto res_rank_opt = Lit::isa(rank);
    assert(res_rank_opt);
    size_t res_rank = *res_rank_opt;

    std::vector<std::optional<int64_t>> res_shape;
    for (size_t i = 0; i < res_rank; ++i) {
        auto dim = S->proj(res_rank, i);
        if (auto lit = Lit::isa(dim))
            res_shape.push_back(static_cast<int64_t>(*lit));
        else
            res_shape.push_back(std::nullopt);
    }
    auto res_elem_type = types_.convert(T);
    MLIRTensorType res_tensor;
    res_tensor.shape = res_shape;
    res_tensor.elem  = std::make_shared<MLIRTypeNode>(res_elem_type);
    MLIRType res_type{std::move(res_tensor)};

    std::vector<MLIRValue> ins;
    for (size_t i = 0; i < n_inputs; ++i)
        ins.push_back(get_or_emit(proj_input(i), into));

    // Output buffer
    std::string buf_name = fresh_name(app) + ".buf";
    MLIRValue out_buf{buf_name, res_type};
    into.ops.emplace_back(std::make_unique<TensorEmptyOp>(out_buf));

    std::vector<MLIRValue> outs{out_buf};

    // Affine maps
    size_t total_dims = res_rank;
    for (size_t i = 0; i < n_inputs; ++i) {
        auto sub_i        = proj_sub(i);
        auto sub_rank_opt = Lit::isa(sub_i->type()->isa<Arr>()->arity());
        assert(sub_rank_opt);
        for (size_t j = 0; j < *sub_rank_opt; ++j) {
            auto idx = sub_i->proj(*sub_rank_opt, j);
            if (auto lit = Lit::isa(idx)) total_dims = std::max(total_dims, (size_t)(*lit + 1));
        }
    }

    auto make_map = [&](std::vector<size_t> dims) -> std::string {
        std::string a, b;
        for (size_t i = 0; i < total_dims; ++i)
            a += (i ? ", " : "") + std::format("d{}", i);
        for (size_t i = 0; i < dims.size(); ++i)
            b += (i ? ", " : "") + std::format("d{}", dims[i]);
        return std::format("affine_map<({}) -> ({})>", a, b);
    };

    std::vector<std::string> indexing_maps;
    for (size_t i = 0; i < n_inputs; ++i) {
        auto sub_i        = proj_sub(i);
        auto sub_rank_opt = Lit::isa(sub_i->type()->isa<Arr>()->arity());
        std::vector<size_t> dims;
        for (size_t j = 0; j < *sub_rank_opt; ++j)
            dims.push_back((size_t)(*Lit::isa(sub_i->proj(*sub_rank_opt, j))));
        indexing_maps.push_back(make_map(dims));
    }
    std::vector<size_t> out_dims;
    for (size_t i = 0; i < res_rank; ++i)
        out_dims.push_back(i);
    indexing_maps.push_back(make_map(out_dims));

    // Iterator types
    std::set<size_t> out_dim_set(out_dims.begin(), out_dims.end());
    std::vector<std::string> iterator_types;
    for (size_t i = 0; i < total_dims; ++i)
        iterator_types.push_back(out_dim_set.count(i) ? "parallel" : "reduction");

    std::vector<MLIRValue> body_args;

    auto* body_var     = body_lam->var();
    auto body_var_type = body_var->type();

    std::vector<size_t> arg_path;
    const Def* arg_type = body_var_type;
    if (auto sigma = body_var_type->isa<Sigma>()) {
        for (size_t i = 0; i < sigma->num_ops(); ++i) {
            if (!sigma->op(i)->isa<Pi>() && !Axm::isa<plug::mem::M>(sigma->op(i))) {
                arg_path.push_back(i);
                arg_type = sigma->op(i);
                break;
            }
        }
    }

    std::map<std::vector<size_t>, MLIRValue> path_to_val;
    MLIRValue acc_val;
    bool have_acc = false;

    auto put = [&](std::vector<size_t> p, MLIRValue v) { path_to_val[std::move(p)] = std::move(v); };

    auto plan_ins = [&](const Def* ins_t, std::vector<size_t> ins_path) {
        if (auto s = ins_t->isa<Sigma>()) {
            for (size_t i = 0; i < s->num_ops(); ++i) {
                auto p = ins_path;
                p.push_back(i);
                MLIRValue v{fresh_name("%in_"), types_.convert(s->op(i))};
                body_args.push_back(v);
                put(std::move(p), v);
            }
        } else if (auto a = ins_t->isa<Arr>()) {
            if (auto n = Lit::isa(a->arity())) {
                for (size_t i = 0; i < *n; ++i) {
                    auto p = ins_path;
                    p.push_back(i);
                    MLIRValue v{fresh_name("%in_"), types_.convert(a->body())};
                    body_args.push_back(v);
                    put(std::move(p), v);
                }
            }
        } else if (!ins_t->isa<Pi>()) {
            MLIRValue v{fresh_name("%in_"), types_.convert(ins_t)};
            body_args.push_back(v);
            put(std::move(ins_path), v);
        }
    };

    if (auto s = arg_type->isa<Sigma>()) {
        // [acc, ins]
        auto acc_p = arg_path;
        acc_p.push_back(0);
        auto ins_p = arg_path;
        ins_p.push_back(1);
        plan_ins(s->op(1), std::move(ins_p));
        acc_val  = MLIRValue{fresh_name("%acc_"), res_elem_type};
        have_acc = true;
        put(std::move(acc_p), acc_val);
    } else if (auto a = arg_type->isa<Arr>()) {
        // «2; T» from a [T, «1; T»] singleton collapse: [acc, single_input]
        if (auto n = Lit::isa(a->arity()); n && *n == 2) {
            auto acc_p = arg_path;
            acc_p.push_back(0);
            auto ins_p = arg_path;
            ins_p.push_back(1);
            MLIRValue v{fresh_name("%in_"), types_.convert(a->body())};
            body_args.push_back(v);
            put(std::move(ins_p), v);
            acc_val  = MLIRValue{fresh_name("%acc_"), res_elem_type};
            have_acc = true;
            put(std::move(acc_p), acc_val);
        } else {
            acc_val  = MLIRValue{fresh_name("%acc_"), res_elem_type};
            have_acc = true;
            put(arg_path, acc_val);
        }
    } else {
        acc_val  = MLIRValue{fresh_name("%acc_"), res_elem_type};
        have_acc = true;
        put(arg_path, acc_val);
    }

    if (have_acc) body_args.push_back(acc_val);

    auto type_arity = [](const Def* t) -> size_t {
        if (auto s = t->isa<Sigma>()) return s->num_ops();
        if (auto a = t->isa<Arr>())
            if (auto n = Lit::isa(a->arity())) return *n;
        return 0;
    };
    auto nav_path = [&](const Def* root, const std::vector<size_t>& path) -> const Def* {
        const Def* cur = root;
        for (size_t i : path) {
            size_t arity = type_arity(cur->type());
            if (arity == 0) return nullptr;
            cur = cur->proj(arity, i);
        }
        return cur;
    };

    for (auto& [path, val] : path_to_val)
        if (auto d = nav_path(body_var, path)) values_[d] = val;

    DefSet body_visited;
    std::function<void(const Def*)> walk_body = [&](const Def* d) {
        if (!body_visited.insert(d).second) return;
        if (d->isa<Var>()) return;
        if (d->isa_mut<Lam>()) return;

        if (d->isa<Extract>()) {
            std::vector<size_t> path;
            const Def* cur = d;
            bool ok        = true;
            while (auto exi = cur->isa<Extract>()) {
                auto lit = Lit::isa(exi->index());
                if (!lit) {
                    ok = false;
                    break;
                }
                path.insert(path.begin(), static_cast<size_t>(*lit));
                cur = exi->tuple();
            }
            if (ok && cur == body_var) {
                if (auto it = path_to_val.find(path); it != path_to_val.end()) values_[d] = it->second;
            }
        }
        for (auto op : d->ops())
            walk_body(op);
    };
    walk_body(body_lam->body());

    DefSet pre_body_keys;
    for (auto& [d, _] : values_)
        pre_body_keys.insert(d);

    auto* op = new LinalgGenericOp(ins, outs, indexing_maps, iterator_types, body_args);
    emit_linalg_body(body_lam, op->body().entry());

    std::vector<const Def*> body_added;
    for (auto& [d, _] : values_)
        if (!pre_body_keys.contains(d)) body_added.push_back(d);
    for (auto* d : body_added)
        values_.erase(d);

    values_[app] = op->result();
    into.ops.emplace_back(op);
}

void MLIREmitter::emit_linalg_body(Lam* body_lam, MLIRBlock& body_bb) {
    assert(body_lam->is_set());
    auto* app = body_lam->body()->isa<App>();
    assert(app);
    auto* callee = app->callee();

    if (is_return_callee(callee, body_lam->ret_var())) {
        std::vector<MLIRValue> yield_vals;
        auto* arg = app->arg();
        if (!Axm::isa<plug::mem::M>(arg->type())) {
            auto v = get_or_emit(arg, body_bb);
            if (!v.empty()) yield_vals.push_back(v);
        }
        body_bb.ops.emplace_back(std::make_unique<LinalgYieldOp>(std::move(yield_vals)));
        return;
    }

    std::cerr << "unhandled callee in emit_linalg_body: " << callee->sym().str() << "\n";
    assert(false && "unhandled callee in emit_linalg_body");
}

// emit_def

MLIRValue MLIREmitter::emit_def(const Def* def, MLIRBlock& into) {
    if (def->isa<Var>()) {
        assert(false && "Var not pre-seeded in values_");
        return {};
    }

    if (auto lit = def->isa<Lit>()) {
        auto mlir_type = types_.convert(lit->type());
        auto name      = fresh_name(lit);
        MLIRAttr attr;
        if (std::holds_alternative<MLIRIndexType>(mlir_type))
            attr = IndexAttr{static_cast<int64_t>(lit->get<uint64_t>())};
        else if (std::holds_alternative<MLIRIntType>(mlir_type))
            attr = IntAttr{static_cast<int64_t>(lit->get<uint64_t>()), mlir_type};
        else if (std::holds_alternative<MLIRFloatType>(mlir_type))
            attr = FloatAttr{lit->get<double>(), mlir_type};
        else
            assert(false && "unhandled literal type");
        MLIRValue result{name, mlir_type};
        into.ops.emplace_back(std::make_unique<ConstantOp>(result, attr));
        return result;
    }

    // App - arith & map_reduce
    if (auto app = def->isa<App>()) {
        namespace core = plug::core;

        if (auto wrap = Axm::isa<core::wrap>(app)) {
            auto [a, b]      = wrap->args<2>([this, &into](auto d) { return get_or_emit(d, into); });
            auto result_type = types_.convert(def->type());
            BinaryIntOp::Kind kind;
            switch (wrap.id()) {
                case core::wrap::add: kind = BinaryIntOp::Kind::Add; break;
                case core::wrap::sub: kind = BinaryIntOp::Kind::Sub; break;
                case core::wrap::mul: kind = BinaryIntOp::Kind::Mul; break;
                case core::wrap::shl: kind = BinaryIntOp::Kind::Shl; break;
                default: assert(false && "unhandled core.wrap op");
            }
            MLIRValue result{fresh_name(def), result_type};
            into.ops.emplace_back(std::make_unique<BinaryIntOp>(result, kind, a, b));
            return result;
        }

        if (auto div = Axm::isa<core::div>(app)) {
            auto [a, b]      = div->args<2>([this, &into](auto d) { return get_or_emit(d, into); });
            auto result_type = types_.convert(def->type());
            BinaryIntOp::Kind kind;
            switch (div.id()) {
                case core::div::sdiv: kind = BinaryIntOp::Kind::DivS; break;
                case core::div::udiv: kind = BinaryIntOp::Kind::DivU; break;
                case core::div::srem: kind = BinaryIntOp::Kind::RemS; break;
                case core::div::urem: kind = BinaryIntOp::Kind::RemU; break;
                default: assert(false && "unhandled core.div op");
            }
            MLIRValue result{fresh_name(def), result_type};
            into.ops.emplace_back(std::make_unique<BinaryIntOp>(result, kind, a, b));
            return result;
        }

        if (auto arith = Axm::isa<plug::math::arith>(app)) {
            auto [a, b]      = arith->args<2>([this, &into](auto d) { return get_or_emit(d, into); });
            auto result_type = types_.convert(def->type());
            BinaryFloatOp::Kind kind;
            switch (arith.id()) {
                case plug::math::arith::add: kind = BinaryFloatOp::Kind::Add; break;
                case plug::math::arith::sub: kind = BinaryFloatOp::Kind::Sub; break;
                case plug::math::arith::mul: kind = BinaryFloatOp::Kind::Mul; break;
                case plug::math::arith::div: kind = BinaryFloatOp::Kind::Div; break;
                case plug::math::arith::rem: kind = BinaryFloatOp::Kind::Rem; break;
                default: assert(false && "unhandled math.arith op");
            }
            MLIRValue result{fresh_name(def), result_type};
            into.ops.emplace_back(std::make_unique<BinaryFloatOp>(result, kind, a, b));
            return result;
        }

        if (auto icmp = Axm::isa<core::icmp>(app)) {
            auto [a, b] = icmp->args<2>([this, &into](auto d) { return get_or_emit(d, into); });
            MLIRType i1{MLIRIntType{1}};
            CmpiOp::Pred pred;
            switch (icmp.id()) {
                case core::icmp::e: pred = CmpiOp::Pred::Eq; break;
                case core::icmp::ne: pred = CmpiOp::Pred::Ne; break;
                case core::icmp::sl: pred = CmpiOp::Pred::Slt; break;
                case core::icmp::sle: pred = CmpiOp::Pred::Sle; break;
                case core::icmp::sg: pred = CmpiOp::Pred::Sgt; break;
                case core::icmp::sge: pred = CmpiOp::Pred::Sge; break;
                case core::icmp::ul: pred = CmpiOp::Pred::Ult; break;
                case core::icmp::ule: pred = CmpiOp::Pred::Ule; break;
                case core::icmp::ug: pred = CmpiOp::Pred::Ugt; break;
                case core::icmp::uge: pred = CmpiOp::Pred::Uge; break;
                default: assert(false && "unhandled core.icmp pred");
            }
            MLIRValue result{fresh_name(def), i1};
            into.ops.emplace_back(std::make_unique<CmpiOp>(result, pred, a, b));
            return result;
        }
        // math::tri → MathUnaryOp
        if (auto tri = Axm::isa<plug::math::tri>(def)) {
            auto a = get_or_emit(app->arg(), into);
            auto t = types_.convert(def->type());
            MathUnaryOp::Kind kind;
            switch (tri.id()) {
                case plug::math::tri::tanh: kind = MathUnaryOp::Kind::Tanh; break;
                case plug::math::tri::sin: kind = MathUnaryOp::Kind::Sin; break;
                case plug::math::tri::cos: kind = MathUnaryOp::Kind::Cos; break;
                default: assert(false && "unhandled math.tri");
            }
            MLIRValue result{fresh_name(def), t};
            into.ops.emplace_back(std::make_unique<MathUnaryOp>(result, kind, a));
            return result;
        }

        // math::extrema → BinaryFloatOp
        if (auto extr = Axm::isa<plug::math::extrema>(def)) {
            auto [a, b] = extr->args<2>([this, &into](auto d) { return get_or_emit(d, into); });
            auto t      = types_.convert(extr->type());
            BinaryFloatOp::Kind kind;
            switch (extr.id()) {
                case plug::math::extrema::fmax: kind = BinaryFloatOp::Kind::MaxNum; break;
                case plug::math::extrema::ieee754max: kind = BinaryFloatOp::Kind::Maximum; break;
                case plug::math::extrema::fmin: kind = BinaryFloatOp::Kind::MinNum; break;
                case plug::math::extrema::ieee754min: kind = BinaryFloatOp::Kind::Minimum; break;
                default: assert(false && "unhandled math.extrema");
            }
            MLIRValue result{fresh_name(def), t};
            into.ops.emplace_back(std::make_unique<BinaryFloatOp>(result, kind, a, b));
            return result;
        }

        if (auto nat_op = Axm::isa<core::nat>(app)) {
            auto* arg        = app->arg();
            auto a           = get_or_emit(arg->proj(2, 0), into);
            auto b           = get_or_emit(arg->proj(2, 1), into);
            auto result_type = types_.convert(def->type());
            BinaryIntOp::Kind kind;
            switch (nat_op.id()) {
                case core::nat::add: kind = BinaryIntOp::Kind::Add; break;
                case core::nat::mul: kind = BinaryIntOp::Kind::Mul; break;
                default: assert(false && "unhandled core.nat op");
            }
            MLIRValue result{fresh_name(def), result_type};
            into.ops.emplace_back(std::make_unique<BinaryIntOp>(result, kind, a, b));
            return result;
        }

        // mem ops — no MLIR value
        if (Axm::isa<plug::mem::M>(def->type())) return {};

        if (Axm::isa<plug::tensor::map_reduce>(app)) {
            emit_linalg_generic(app, into);
            return values_[def];
        }

        if (auto bc = Axm::isa<plug::tensor::broadcast>(app)) {
            // mirrors lower_broadcast extraction exactly
            auto [s_in, s_out, input] = bc->arg()->projs<3>();
            auto callee               = bc->callee()->as<App>();
            auto [T, r]               = callee->args<2>();

            auto r_lit = Lit::isa(r);
            assert(r_lit && "broadcast rank must be literal");
            auto r_nat = *r_lit;

            // derive broadcast dimensions: where s_in dim == 1
            std::vector<int64_t> bcast_dims;
            for (size_t i = 0; i < r_nat; ++i) {
                auto dim_in = s_in->proj(r_nat, i);
                if (auto lit = Lit::isa(dim_in); lit && *lit == 1) bcast_dims.push_back(static_cast<int64_t>(i));
            }

            auto in_val   = get_or_emit(input, into);
            auto out_type = types_.convert(def->type());

            // tensor.empty for output buffer
            std::string buf_name = fresh_name(def) + ".buf";
            MLIRValue out_buf{buf_name, out_type};
            into.ops.emplace_back(std::make_unique<TensorEmptyOp>(out_buf));

            MLIRValue result{fresh_name(def), out_type};
            into.ops.emplace_back(std::make_unique<LinalgBroadcastOp>(result, in_val, out_buf, std::move(bcast_dims)));
            return result;
        }
    }

    if (auto ex = def->isa<Extract>()) {
        auto sym = ex->sym().str();
        if (!sym.empty()) {
            for (auto& [d, v] : values_)
                if (v.name == "%" + sym) return v;
        }

        // index-based: resolve through projection chain
        if (auto lit_idx = Lit::isa(ex->index())) {
            size_t i     = static_cast<size_t>(*lit_idx);
            auto* tuple  = ex->tuple();
            size_t arity = 0;
            if (auto s = tuple->type()->isa<Sigma>())
                arity = s->num_ops();
            else if (auto a = tuple->type()->isa<Arr>())
                if (auto n = Lit::isa(a->arity())) arity = *n;

            if (arity > 0) {
                auto proj = tuple->proj(arity, i);
                if (proj != def) {
                    if (auto it = values_.find(proj); it != values_.end()) return it->second;
                    // recurse — proj is a different node, won't loop
                    auto v = emit_def(proj, into);
                    if (!v.empty()) return v;
                }
            }
        }
        assert(false && "Extract not seeded");
        return {};
    }

    if (auto tup = def->isa<Tuple>()) {
        auto mlir_type = types_.convert(def->type());
        auto name      = fresh_name(def);

        std::function<void(const Def*, std::vector<double>&)> collect_vals;
        collect_vals = [&](const Def* d, std::vector<double>& out) {
            if (auto lit = d->isa<Lit>()) {
                out.push_back(mim::bitcast<double>(lit->get<u64>()));
                return;
            }
            if (auto inner = d->isa<Tuple>()) {
                for (size_t i = 0; i < inner->num_ops(); ++i)
                    collect_vals(inner->op(i), out);
                return;
            }
            if (auto pack = d->isa<Pack>()) {
                if (auto n = Lit::isa(pack->arity())) {
                    for (size_t i = 0; i < *n; ++i)
                        collect_vals(pack->body(), out);
                    return;
                }
            }
            assert(false && "unexpected node in literal tensor");
        };

        auto& tt = std::get<MLIRTensorType>(mlir_type);
        std::vector<double> flat_vals;
        collect_vals(tup, flat_vals);
        std::string dense_str = make_dense_attr(flat_vals, tt);

        MLIRValue result{name, mlir_type};
        into.ops.emplace_back(std::make_unique<DenseConstOp>(result, std::move(dense_str)));
        return result;
    }

    std::cerr << "unhandled def: " << def->node_name() << " sym='" << def->sym().str() << "'"
              << " type=" << def->type()->node_name() << "\n";
    assert(false && "unhandled def in emit_def");
    return {};
}

} // namespace mim::mlir_be
