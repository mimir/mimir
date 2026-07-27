#include "mim/plug/ll/ll.h"

#include <fstream>
#include <iomanip>
#include <ranges>
#include <string>

#include <mim/config.h>
#include <mim/phase.h>
#include <mim/plugin.h>

#include "mim/plug/core/core.h"
#include "mim/plug/ll/autogen.h"

namespace mim::plug::ll {

using namespace std::string_literals;

namespace clos = mim::plug::clos;
namespace core = mim::plug::core;
namespace vec  = mim::plug::vec;

/// Pipeline phase for `%ll.emit`.
/// Writes the LLVM IR of the fully lowered world to `<world>.ll` (or `a.ll` if the world is unnamed).
/// The output path can be overridden on the command line via `-X ll:o=<file>` or `-X ll:output=<file>`.
/// The runtime-wrapper linking mode is selected via `-X ll:rt=embed` (default) or `-X ll:rt=extern`.
class Emit : public Phase {
public:
    Emit(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? std::string(world().name().view()) : "a"s;
        auto path = name + ".ll"s;
        auto rt   = Emitter::Rt::embed;
        for (const auto& arg : args()) {
            world().DLOG("ll backend arg: `{}`", arg);
            if (arg.starts_with("o="))
                path = arg.substr(2);
            else if (arg.starts_with("output="))
                path = arg.substr(7);
            else if (arg == "rt=embed")
                rt = Emitter::Rt::embed;
            else if (arg == "rt=extern")
                rt = Emitter::Rt::ext;
        }
        auto ofs     = std::ofstream(path);
        auto emitter = Emitter(world(), "llvm_emitter", ofs);
        emitter.rt_mode(rt);
        if (rt == Emitter::Rt::embed) emitter.load_rt_module("ll_rt.ll");
        emitter.run();
    }
};

/*
 * Heavy, target-independent emitter methods.
 * These live here (compiled once into libmim_ll) instead of the header so that
 * libmim_ll_nvptx does not recompile them; ll_nvptx reaches them via the
 * `extern "C"` shims below, looked up with GET_FUN_PTR (see ll.h).
 */

std::string Emitter::convert_impl(const Def* type, bool simd) {
    if (auto i = types_.find(type); i != types_.end()) return i->second;

    if (Axm::isa<mem::M>(type)) fe::throwf("ll backend: cannot convert %mem.M type '{}'", type);
    std::ostringstream s;
    std::string name;

    if (type->isa<Nat>()) {
        return types_[type] = "i64";
    } else if (Idx::isa(type)) {
        return types_[type] = "i" + std::to_string(Idx::expect_bitwidth(type, "a statically-sized index type"));
    } else if (auto w = math::isa_f(type)) {
        switch (*w) {
            case 16: return types_[type] = "half";
            case 32: return types_[type] = "float";
            case 64: return types_[type] = "double";
            default: fe::throwf("ll backend: unsupported floating-point width {} in type '{}'", *w, type);
        }
    } else if (auto ptr = Axm::isa<mem::Ptr>(type)) {
        auto [pointee, addr_space] = ptr->args<2>();
        std::print(s, "{} addrspace({})*", convert(pointee, false), addr_space);
    } else if (auto arr = type->isa<Arr>()) {
        if (auto se = is_simd(arr); se && simd) {
            auto [size, elem] = *se;
            std::print(s, "<{} x {}>", size, convert(elem));
        } else {
            u64 size = 0;
            if (auto arity = Lit::isa(arr->arity())) size = *arity;
            std::print(s, "[{} x {}]", size, convert(arr->body(), false));
        }
    } else if (auto pi = type->isa<Pi>()) {
        if (!Pi::isa_returning(pi)) fe::throwf("ll backend: cannot convert the type of a basic block: '{}'", pi);
        std::print(s, "{} (", convert_ret_pi(pi->ret_pi()));

        if (auto t = detail::isa_mem_sigma_2(pi->dom()))
            s << convert(t);
        else {
            auto doms = pi->doms();
            for (auto sep = ""; auto dom : doms.view().rsubspan(1)) {
                if (Axm::isa<mem::M>(dom)) continue;
                s << sep << convert(dom);
                sep = ", ";
            }
        }
        s << ")*";
    } else if (auto t = detail::isa_mem_sigma_2(type)) {
        return convert(t);
    } else if (auto sigma = type->isa<Sigma>()) {
        if (sigma->isa_mut()) {
            name          = id(sigma);
            types_[sigma] = name;
            std::print(s, "{} = type", name);
        }

        std::print(s, "{{");
        for (auto sep = ""; auto t : sigma->ops()) {
            if (Axm::isa<mem::M>(t)) continue;
            s << sep << convert(t);
            sep = ", ";
        }
        std::print(s, "}}");
    } else {
        fe::throwf("ll backend: cannot convert type '{}' to LLVM", type);
    }

    if (name.empty()) return types_[type] = s.str();

    if (s.str().empty()) fe::throwf("ll backend: empty type declaration for '{}'", type);
    type_decls_ << s.str() << '\n';
    return types_[type] = name;
}

void Emitter::finalize_impl() {
    for (auto& [lam, bb] : lam2bb_) {
        for (const auto& [phi, args] : bb.phis) {
            std::print(bb.head().emplace_back(), "{} = phi {} ", id(phi), convert(phi->type()));
            for (auto sep = ""; const auto& [arg, pred] : args) {
                std::print(bb.head().back(), "{}[ {}, {} ]", sep, arg, pred);
                sep = ", ";
            }
        }
    }

    for (auto mut : Scheduler::schedule(nest())) {
        if (auto lam = mut->isa_mut<Lam>()) {
            if (!lam2bb_.contains(lam)) fe::throwf("ll backend: no basic block was emitted for '{}'", lam);
            auto& bb = lam2bb_[lam];
            std::print(func_impls_, "{}:\n", lam->unique_name());

            ++tab;
            for (const auto& part : bb.parts)
                for (const auto& line : part)
                    std::println(func_impls_, "{}{}", tab, line.str());
            --tab;
            func_impls_ << std::endl;
        }
    }

    std::print(func_impls_, "}}\n\n");
}

/*
 Block           type              return
BB:
          Cn [M, a, A]         →    2 phi
          Cn «2;A»             →    2 phi
          Cn [M, «2;A»]        →    1 phi
Ret:
          Cn[M,A,A]            →    1 phi
          Cn «2;A»             →    1 phi
          Cn[M, «2;A»]         →    1 phi

Fun:
          Cn[A, A, Cn R]       →    2 args + ret
          Cn[«2; A», Cn R]     →    1 args + ret
          Cn[M, A, A, Cn R]    →    2 args + ret
          Cn[M, «2; A», Cn R]  →    1 args + ret
*/
void Emitter::emit_epilogue_impl(Lam* lam) {
    auto app = lam->body()->expect<App>("an application in tail position");
    auto& bb = lam2bb_[lam];
    // A target-specific intrinsic in tail position (e.g. %gpu.launch) emits its own code and
    // yields the continuation to branch to.
    if (auto ret = isa_targetspecific_intrinsic(bb, app)) return bb.tail("br label {}", *ret);
    if (app->callee() == root()->ret_var()) { // return
        Vector<std::string> values;
        DefVec types;
        for (auto arg : app->args()) {
            if (auto val = emit_unsafe(arg); !val.empty()) {
                values.emplace_back(val);
                types.emplace_back(arg->type());
            }
        }

        switch (values.size()) {
            case 0: return bb.tail("ret void");
            case 1:
                return Axm::isa<mem::M>(types[0]) ? bb.tail("ret void")
                                                  : bb.tail("ret {} {}", convert(types[0]), values[0]);
            default: {
                std::string type;
                std::string prev;

                if (auto se = is_simd_aggregate(types)) {
                    auto common_src = find_common_simd_src(app);
                    if (common_src) {
                        auto v_src = emit(common_src);
                        auto t     = convert(common_src->type());
                        return bb.tail("ret {} {}", t, v_src);
                    }
                    auto [size, elem] = *se;
                    auto val_t        = convert(elem);

                    type = std::format("<{} x {}>", size, val_t);
                    for (auto val : values) {
                        if (prev.empty())
                            prev = "<";
                        else
                            prev += ", ";
                        prev += std::format("{} {}", val_t, val);
                    }
                    prev += ">";
                } else {
                    prev = "undef";
                    type = convert(world().sigma(types));
                    for (size_t i = 0, n = values.size(); i != n; ++i) {
                        if (auto mem = Axm::isa<mem::M>(types[i])) continue;
                        auto v_elem = values[i];
                        auto t_elem = convert(types[i]);
                        auto namei  = "%ret_val." + std::to_string(i);
                        bb.tail("{} = insertvalue {} {}, {} {}, {}", namei, type, prev, t_elem, v_elem, i);
                        prev = namei;
                    }
                }
                bb.tail("ret {} {}", type, prev);
            }
        }

    } else if (auto dispatch = Dispatch(app)) {
        for (auto callee : dispatch.tuple()->projs([](const Def* def) { return def->isa_mut<Lam>(); }))
            if (size_t n = callee->num_tvars(); n == 1 && is_simd(callee->var(0)->type()))
                emit_phi(callee, callee->var(0), emit(app->arg(n, 0)), lam);
            else
                emit_phi_args(callee, app, lam);

        auto v_index = emit(dispatch.index());
        size_t n     = dispatch.num_targets();
        auto bbs     = absl::FixedArray<std::string>(n);
        for (size_t i = 0; i != n; ++i)
            bbs[i] = emit(dispatch.target(i));

        if (auto branch = Branch(app)) return bb.tail("br i1 {}, label {}, label {}", v_index, bbs[1], bbs[0]);

        auto t_index = convert(dispatch.index()->type());
        bb.tail("switch {} {}, label {} [ ", t_index, v_index, bbs[0]);
        for (size_t i = 1; i != n; ++i)
            std::print(bb.tail().back(), "{} {}, label {} ", t_index, std::to_string(i), bbs[i]);
        std::print(bb.tail().back(), "]");
    } else if (app->callee()->isa<Bot>()) {
        return bb.tail("ret ; bottom: unreachable");
    } else if (auto callee = Lam::isa_mut_basicblock(app->callee())) { // ordinary jump

        if (auto common_src = find_common_simd_src(app)) {
            auto v_src      = emit(common_src);
            auto callee_var = callee->var();
            if (simd_phi_.find(callee) == simd_phi_.end()) simd_phi_[callee] = callee_var;
            auto key = simd_phi_[callee];
            emit_phi(callee, key, v_src, lam);
            for (auto var : callee->vars())
                locals_[var] = id(key);
            locals_[callee_var] = id(key);
        } else {
            emit_phi_args(callee, app, lam);
        }
        return bb.tail("br label {}", id(callee));

    } else if (auto longjmp = Axm::isa<clos::longjmp>(app)) {
        declare("void @longjmp(i8*, i32) noreturn");

        auto [mem, jbuf, tag] = app->args<3>();
        emit_unsafe(mem);
        auto v_jb  = emit(jbuf);
        auto v_tag = emit(tag);
        bb.tail("call void @longjmp(i8* {}, i32 {})", v_jb, v_tag);
        return bb.tail("unreachable");
    } else if (auto mslot = Axm::isa<mem::mslot>(app)) {
        // Continuation-based stack slot: allocate and jump to the passed continuation with the fresh pointer.
        auto [Ta, rest]            = mslot->uncurry_args<2>();
        auto [pointee, addr_space] = Ta->projs<2>();
        auto [msize, ret]          = rest->projs<2>();
        emit_unsafe(msize->proj(0)); // mem
        // TODO array with size
        auto ret_lam = ret->expect_mut<Lam>("a %mem.slot continuation");
        auto ptr     = ret_lam->var(2, 1);
        auto v_ptr   = emit_slot(bb, app, pointee, addr_space);
        emit_phi(ret_lam, ptr, v_ptr, lam);
        return bb.tail("br label {}", id(ret_lam));
    } else if (Pi::isa_returning(app->callee_type())) { // function call
        auto v_callee = emit(app->callee());

        Vector<std::string> args;
        auto app_args = app->args();
        for (auto arg : app_args.view().rsubspan(1))
            if (auto v_arg = emit_unsafe(arg); !v_arg.empty()) args.emplace_back(convert(arg->type()) + " " + v_arg);

        if (app->args().back()->isa<Bot>()) {
            // TODO: Perhaps it'd be better to simply η-wrap this prior to the BE...
            if (convert_ret_pi(app->callee_type()->ret_pi()) != "void")
                fe::throwf("ll backend: call with a ⊥ return continuation must return void, but '{}' does not", app);
            bb.tail("call void {}({})", v_callee, fe::Join(args));
            return bb.tail("unreachable");
        }

        auto ret_lam = app->args().back()->expect_mut<Lam>("a return continuation");
        size_t n     = 0;
        for (auto var : ret_lam->vars())
            if (!Axm::isa<mem::M>(var->type())) ++n;

        if (n == 0) {
            bb.tail("call void {}({})", v_callee, fe::Join(args));
        } else {
            auto name  = "%" + app->unique_name() + "ret";
            auto t_ret = convert_ret_pi(ret_lam->type());
            bb.tail("{} = call {} {}({})", name, t_ret, v_callee, fe::Join(args));
            emit_phi(ret_lam, ret_lam->var(), name, lam);
        }

        return bb.tail("br label {}", id(ret_lam));
    }
}

std::string Emitter::emit_bb_impl(BB& bb, const Def* def) {
    if (auto lam = def->isa<Lam>()) return id(lam);

    auto name = id(def);
    std::string op;

    auto emit_tuple = [&](const Def* tuple) {
        if (detail::isa_mem_sigma_2(tuple->type())) {
            emit_unsafe(tuple->proj(2, 0));
            return emit(tuple->proj(2, 1));
        }

        if (tuple->is_closed()) {
            bool is_array   = tuple->type()->isa<Arr>();
            auto simd_array = convert(tuple->type()).front() == '<'; // needed to respect pointer context
            std::string s;
            s += simd_array ? "<" : is_array ? "[" : "{";
            auto sep = "";
            for (size_t i = 0, n = tuple->num_projs(); i != n; ++i) {
                auto e = tuple->proj(n, i);
                if (auto v_elem = emit_unsafe(e); !v_elem.empty()) {
                    auto t_elem = convert(e->type());
                    s += sep + t_elem + " " + v_elem;
                    sep = ", ";
                }
            }

            return s += simd_array ? ">" : is_array ? "]" : "}";
        }

        std::string prev = "undef";
        auto t           = convert(tuple->type());
        for (size_t src = 0, dst = 0, n = tuple->num_projs(); src != n; ++src) {
            auto e = tuple->proj(n, src);
            if (auto elem = emit_unsafe(e); !elem.empty()) {
                auto elem_t = convert(e->type());
                // TODO: check dst vs src
                auto namei = name + "." + std::to_string(dst);
                if (t.front() == '<') // not using is_simd to respect the pointer context (Pointer Pointee case)
                    prev = bb.assign(namei, "insertelement {} {}, {} {}, {} {}", t, prev, elem_t, elem, elem_t, dst);
                else
                    prev = bb.assign(namei, "insertvalue {} {}, {} {}, {}", t, prev, elem_t, elem, dst);
                dst++;
            }
        }
        return prev;
    };

    if (def->isa<Var>()) {
        if (is_simd(def->type())) return id(def);
        auto ts = def->type()->projs();
        if (std::ranges::any_of(ts, [](auto t) { return Axm::isa<mem::M>(t); })) return {};
        return emit_tuple(def);
    }

    auto emit_gep_index = [&](const Def* index) {
        auto v_i = emit(index);
        auto t_i = convert(index->type());

        if (auto size = Idx::isa(index->type())) {
            if (auto w = Idx::size2bitwidth(size); w && *w < 64) {
                v_i = bb.assign(name + ".zext",
                                "zext {} {} to i{} ; add one more bit for gep index as it is treated as signed value",
                                t_i, v_i, *w + 1);
                t_i = "i" + std::to_string(*w + 1);
            }
        }

        return std::pair(v_i, t_i);
    };

    if (auto lit = def->isa<Lit>()) {
        if (lit->type()->isa<Nat>() || Idx::isa(lit->type())) {
            return std::to_string(lit->get());
        } else if (auto w = math::isa_f(lit->type())) {
            std::stringstream s;
            u64 hex;

            switch (*w) {
                case 16:
                    s << "0xH" << std::setfill('0') << std::setw(4) << std::right << std::hex << lit->get<u16>();
                    return s.str();
                case 32: {
                    hex = std::bit_cast<u64>(f64(lit->get<f32>()));
                    break;
                }
                case 64: hex = lit->get<u64>(); break;
                default: fe::throwf("ll backend: unsupported floating-point width {} for literal '{}'", *w, def);
            }

            s << "0x" << std::setfill('0') << std::setw(16) << std::right << std::hex << hex;
            return s.str();
        }
        fe::throwf("ll backend: cannot emit literal '{}' of type '{}'", def, def->type());
    } else if (def->isa<Bot>()) {
        return "undef";
    } else if (auto top = def->isa<Top>()) {
        if (Axm::isa<mem::M>(top->type())) return {};
        // bail out to error below
    } else if (auto tuple = def->isa<Tuple>()) {
        return emit_tuple(tuple);
    } else if (auto pack = def->isa<Pack>()) {
        if (auto lit = Lit::isa(pack->body()); lit && *lit == 0) return "zeroinitializer";
        return emit_tuple(pack);
    } else if (auto sel = Select(def)) {
        auto t                = convert(sel.extract()->type());
        auto [elem_a, elem_b] = sel.pair()->projs<2>([&](auto e) { return emit_unsafe(e); });
        auto cond_t           = convert(sel.cond()->type());
        auto cond             = emit(sel.cond());
        return bb.assign(name, "select {} {}, {} {}, {} {}", cond_t, cond, t, elem_b, t, elem_a);
    } else if (auto extract = def->isa<Extract>()) {
        auto tuple = extract->tuple();
        auto index = extract->index();
        auto v_tup = emit_unsafe(tuple);
        if (is_simd(tuple->type()) && !Axm::isa<mem::M>(tuple->type())) return v_tup;

        // this exact location is important: after emitting the tuple -> ordering of mem ops
        // before emitting the index, as it might be a weird value for mem vars.
        if (Axm::isa<mem::M>(extract->type())) return {};
        if (auto sigma = extract->type()->isa<Sigma>(); sigma && sigma->num_ops() == 0) return {};

        auto t_tup = convert(tuple->type());
        if (auto li = Lit::isa(index)) {
            if (detail::isa_mem_sigma_2(tuple->type())) return v_tup;
            // Adjust index: convert() drops %mem.M elements from sigmas,
            // so subtract the number of mem elements preceding the index.
            auto v_i = *li;
            if (auto sigma = tuple->type()->isa<Sigma>())
                for (u64 i = 0; i < *li; ++i)
                    if (Axm::isa<mem::M>(sigma->op(i))) --v_i;

            return bb.assign(name, "extractvalue {} {}, {}", t_tup, v_tup, v_i);
        }

        auto t_elem     = convert(extract->type());
        auto [v_i, t_i] = emit_gep_index(index);

        std::print(lam2bb_[root()].body().emplace_front(),
                   "{}.alloca = alloca {} ; copy to alloca to emulate extract with store + gep + load", name, t_tup);
        std::print(bb.body().emplace_back(), "store {} {}, {}* {}.alloca", t_tup, v_tup, t_tup, name);
        std::print(bb.body().emplace_back(), "{}.gep = getelementptr inbounds {}, {}* {}.alloca, i64 0, {} {}", name,
                   t_tup, t_tup, name, t_i, v_i);
        return bb.assign(name, "load {}, {}* {}.gep", t_elem, t_elem, name);
    } else if (auto insert = def->isa<Insert>()) {
        if (Axm::isa<mem::M>(insert->tuple()->proj(0)->type()))
            fe::throwf("ll backend: cannot insert into a tuple with a %mem.M element: '{}'", insert);
        auto t_tup = convert(insert->tuple()->type());
        auto t_val = convert(insert->value()->type());
        auto v_tup = emit(insert->tuple());
        auto v_val = emit(insert->value());
        if (auto idx = Lit::isa(insert->index())) {
            auto v_idx = emit(insert->index());
            if (is_simd(insert->tuple()->type()))

                return bb.assign(name, "insertelement {} {}, {} {}, i32 {}", t_tup, v_tup, t_val, v_val, v_idx);
            else

                return bb.assign(name, " insertvalue {} {}, {} {}, {}", t_tup, v_tup, t_val, v_val, v_idx);
        } else {
            if (is_simd(insert->tuple()->type())) {
                auto v_i = emit(insert->index());
                auto t_i = convert(insert->index()->type());
                if (t_i != "i32") {
                    auto w_src = Idx::expect_bitwidth(insert->index()->type(), "an %insert index of known width");
                    v_i        = bb.assign(name + ".idx", "{} {} {} to i32", w_src < 32 ? "zext" : "trunc", t_i, v_i);
                }
                return bb.assign(name, "insertelement {} {}, {} {}, i32 {}", t_tup, v_tup, t_val, v_val, v_i);
            }
            auto t_elem     = convert(insert->value()->type());
            auto [v_i, t_i] = emit_gep_index(insert->index());
            std::print(lam2bb_[root()].body().emplace_front(),
                       "{}.alloca = alloca {} ; copy to alloca to emulate insert with store + gep + load", name, t_tup);
            std::print(bb.body().emplace_back(), "store {} {}, {}* {}.alloca", t_tup, v_tup, t_tup, name);
            std::print(bb.body().emplace_back(), "{}.gep = getelementptr inbounds {}, {}* {}.alloca, i64 0, {} {}",
                       name, t_tup, t_tup, name, t_i, v_i);
            std::print(bb.body().emplace_back(), "store {} {}, {}* {}.gep", t_val, v_val, t_val, name);
            return bb.assign(name, "load {}, {}* {}.alloca", t_tup, t_tup, name);
        }
    } else if (auto global = def->isa<Global>()) {
        auto v_init                = emit(global->init());
        auto [pointee, addr_space] = Axm::expect<mem::Ptr>(global->type(), "a %mem.Ptr")->args<2>();
        std::print(vars_decls_, "{} = global {} {}\n", name, convert(pointee), v_init);
        return globals_[global] = name;
    } else if (auto nat = Axm::isa<core::nat>(def)) {
        auto [a, b] = nat->args<2>([this](auto def) { return emit(def); });

        switch (nat.id()) {
            case core::nat::add: return bb.assign(name, "add nsw nuw i64 {}, {}", a, b);
            case core::nat::sub: {
                // nat subtraction saturates at 0: cap result when b > a
                auto ugt = bb.assign(name + ".ugt", "icmp ugt i64 {}, {}", b, a);
                auto raw = bb.assign(name + ".raw", "sub i64 {}, {}", a, b);
                return bb.assign(name, "select i1 {}, i64 0, i64 {}", ugt, raw);
            }
            case core::nat::mul: return bb.assign(name, "mul nsw nuw i64 {}, {}", a, b);
            // %core.nat.div/mod define division/modulo by zero as `a / 0 = 0` and `a % 0 = a`.
            // replace a zero divisor by 1 to keep udiv/urem well-defined, then select the
            // defined result for the zero case (0 for div, a for mod).
            case core::nat::div: {
                auto bz   = bb.assign(name + ".bz", "icmp eq i64 {}, 0", b);
                auto bsaf = bb.assign(name + ".bsafe", "select i1 {}, i64 1, i64 {}", bz, b);
                auto q    = bb.assign(name + ".q", "udiv i64 {}, {}", a, bsaf);
                return bb.assign(name, "select i1 {}, i64 0, i64 {}", bz, q);
            }
            case core::nat::mod: {
                auto bz   = bb.assign(name + ".bz", "icmp eq i64 {}, 0", b);
                auto bsaf = bb.assign(name + ".bsafe", "select i1 {}, i64 1, i64 {}", bz, b);
                auto r    = bb.assign(name + ".r", "urem i64 {}, {}", a, bsaf);
                return bb.assign(name, "select i1 {}, i64 {}, i64 {}", bz, a, r);
            }
        }
    } else if (auto ncmp = Axm::isa<core::ncmp>(def)) {
        auto [a, b] = ncmp->args<2>([this](auto def) { return emit(def); });
        op          = "icmp ";

        switch (ncmp.id()) {
                // clang-format off
            case core::ncmp::e:  op += "eq" ; break;
            case core::ncmp::ne: op += "ne" ; break;
            case core::ncmp::g:  op += "ugt"; break;
            case core::ncmp::ge: op += "uge"; break;
            case core::ncmp::l:  op += "ult"; break;
            case core::ncmp::le: op += "ule"; break;
            // clang-format on
            default: fe::throwf("ll backend: unhandled %core.ncmp id in '{}'", def);
        }

        return bb.assign(name, "{} i64 {}, {}", op, a, b);
    } else if (auto idx = Axm::isa<core::idx>(def)) {
        auto x = emit(idx->arg());
        auto s = Idx::expect_bitwidth(idx->type(), "a %core.idx result of known width");
        auto t = convert(idx->type());
        if (s < 64) return bb.assign(name, "trunc i64 {} to {}", x, t);
        return x;
    } else if (auto bit1 = Axm::isa<core::bit1>(def)) {
        if (bit1.id() != core::bit1::neg) fe::throwf("ll backend: unhandled %core.bit1 id in '{}'", def);
        auto x = emit(bit1->arg());
        auto t = convert(bit1->type());
        return bb.assign(name, "xor {} -1, {}", t, x);
    } else if (auto bit2 = Axm::isa<core::bit2>(def)) {
        auto [a, b] = bit2->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(bit2->type());

        auto neg = [&](std::string_view x) { return bb.assign(name + ".neg", "xor {} -1, {}", t, x); };

        switch (bit2.id()) {
                // clang-format off
            case core::bit2::and_: return bb.assign(name, "and {} {}, {}", t, a, b);
            case core::bit2:: or_: return bb.assign(name, "or  {} {}, {}", t, a, b);
            case core::bit2::xor_: return bb.assign(name, "xor {} {}, {}", t, a, b);
            case core::bit2::nand: return neg(bb.assign(name, "and {} {}, {}", t, a, b));
            case core::bit2:: nor: return neg(bb.assign(name, "or  {} {}, {}", t, a, b));
            case core::bit2::nxor: return neg(bb.assign(name, "xor {} {}, {}", t, a, b));
            case core::bit2:: iff: return bb.assign(name, "and {} {}, {}", t, neg(a), b);
            case core::bit2::niff: return bb.assign(name, "or  {} {}, {}", t, neg(a), b);
            // clang-format on
            default: fe::throwf("ll backend: unhandled %core.bit2 id in '{}'", def);
        }
    } else if (auto shr = Axm::isa<core::shr>(def)) {
        auto [a, b] = shr->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(shr->type());

        switch (shr.id()) {
            case core::shr::a: op = "ashr"; break;
            case core::shr::l: op = "lshr"; break;
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto wrap = Axm::isa<core::wrap>(def)) {
        auto [mode, ab] = wrap->uncurry_args<2>();
        auto [a, b]     = ab->projs<2>([this](auto def) { return emit(def); });
        auto t          = convert(wrap->type());
        auto lmode      = static_cast<core::Mode>(Lit::expect(mode, "a %core.wrap mode"));

        switch (wrap.id()) {
            case core::wrap::add: op = "add"; break;
            case core::wrap::sub: op = "sub"; break;
            case core::wrap::mul: op = "mul"; break;
            case core::wrap::shl: op = "shl"; break;
        }

        if (fe::has_flag(lmode, core::Mode::nuw)) op += " nuw";
        if (fe::has_flag(lmode, core::Mode::nsw)) op += " nsw";

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto div = Axm::isa<core::div>(def)) {
        auto [m, xy] = div->args<2>();
        auto [x, y]  = xy->projs<2>();
        auto t       = convert(x->type());
        emit_unsafe(m);
        auto a = emit(x);
        auto b = emit(y);

        switch (div.id()) {
            case core::div::sdiv: op = "sdiv"; break;
            case core::div::udiv: op = "udiv"; break;
            case core::div::srem: op = "srem"; break;
            case core::div::urem: op = "urem"; break;
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto icmp = Axm::isa<core::icmp>(def)) {
        auto [a, b] = icmp->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(icmp->arg(0)->type());
        op          = "icmp ";

        switch (icmp.id()) {
                // clang-format off
            case core::icmp::e:   op += "eq" ; break;
            case core::icmp::ne:  op += "ne" ; break;
            case core::icmp::sg:  op += "sgt"; break;
            case core::icmp::sge: op += "sge"; break;
            case core::icmp::sl:  op += "slt"; break;
            case core::icmp::sle: op += "sle"; break;
            case core::icmp::ug:  op += "ugt"; break;
            case core::icmp::uge: op += "uge"; break;
            case core::icmp::ul:  op += "ult"; break;
            case core::icmp::ule: op += "ule"; break;
            // clang-format on
            default: fe::throwf("ll backend: unhandled %core.icmp id in '{}'", def);
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto extr = Axm::isa<core::extrema>(def)) {
        auto [x, y]   = extr->args<2>();
        auto t        = convert(x->type());
        auto a        = emit(x);
        auto b        = emit(y);
        std::string f = "llvm.";
        switch (extr.id()) {
            case core::extrema::Sm: f += "smin."; break;
            case core::extrema::SM: f += "smax."; break;
            case core::extrema::sm: f += "umin."; break;
            case core::extrema::sM: f += "umax."; break;
        }
        f += t;
        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto abs = Axm::isa<core::abs>(def)) {
        auto [m, x]   = abs->args<2>();
        auto t        = convert(x->type());
        auto a        = emit(x);
        std::string f = "llvm.abs." + t;
        declare("{} @{}({}, {})", t, f, t, "i1");
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, "i1", "1");
    } else if (auto conv = Axm::isa<core::conv>(def)) {
        auto v_src = emit(conv->arg());
        auto t_src = convert(conv->arg()->type());
        auto t_dst = convert(conv->type());

        nat_t w_src = Idx::expect_bitwidth(conv->arg()->type(), "a %core.conv source of known width");
        nat_t w_dst = Idx::expect_bitwidth(conv->type(), "a %core.conv target of known width");

        if (w_src == w_dst) return v_src;

        switch (conv.id()) {
            case core::conv::s: op = w_src < w_dst ? "sext" : "trunc"; break;
            case core::conv::u: op = w_src < w_dst ? "zext" : "trunc"; break;
        }

        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto bitcast = Axm::isa<core::bitcast>(def)) {
        auto dst_type_ptr = Axm::isa<mem::Ptr>(bitcast->type());
        auto src_type_ptr = Axm::isa<mem::Ptr>(bitcast->arg()->type());
        auto v_src        = emit(bitcast->arg());
        auto t_src        = convert(bitcast->arg()->type());
        auto t_dst        = convert(bitcast->type());

        if (auto lit = Lit::isa(bitcast->arg()); lit && *lit == 0) return "zeroinitializer";
        // clang-format off
        if (src_type_ptr && dst_type_ptr) return bb.assign(name,  "bitcast {} {} to {}", t_src, v_src, t_dst);
        if (src_type_ptr)                 return bb.assign(name, "ptrtoint {} {} to {}", t_src, v_src, t_dst);
        if (dst_type_ptr)                 return bb.assign(name, "inttoptr {} {} to {}", t_src, v_src, t_dst);
        // clang-format on

        auto size2width = [&](const Def* type) {
            if (type->isa<Nat>()) return 64_n;
            if (Idx::isa(type)) return Idx::expect_bitwidth(type, "a statically-sized index type");
            return 0_n;
        };

        auto src_size = size2width(bitcast->arg()->type());
        auto dst_size = size2width(bitcast->type());

        op = "bitcast";
        if (src_size && dst_size) {
            if (src_size == dst_size) return v_src;
            op = (src_size < dst_size) ? "zext" : "trunc";
        }
        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto lea = Axm::isa<mem::lea>(def)) {
        auto [ptr, i]  = lea->args<2>();
        auto pointee   = Axm::expect<mem::Ptr>(ptr->type(), "a %mem.Ptr")->arg(0);
        auto v_ptr     = emit(ptr);
        auto t_pointee = convert(pointee);
        auto t_ptr     = convert(ptr->type());
        if (pointee->isa<Sigma>())
            return bb.assign(name, "getelementptr inbounds {}, {} {}, i64 0, i32 {}", t_pointee, t_ptr, v_ptr,
                             Lit::expect(i, "a struct-field index"));

        if (!pointee->isa<Arr>()) fe::throwf("ll backend: %mem.lea on a pointer to a non-aggregate '{}'", pointee);
        auto [v_i, t_i] = emit_gep_index(i);

        return bb.assign(name, "getelementptr inbounds {}, {} {}, i64 0, {} {}", t_pointee, t_ptr, v_ptr, t_i, v_i);
    } else if (auto malloc = Axm::isa<mem::malloc>(def)) {
        auto address_space = malloc->decurry()->arg(1);
        if (Lit::expect(address_space, "an address space") != 0)
            if (auto target_specific = isa_targetspecific_intrinsic(bb, def)) return target_specific.value();

        declare("i8* @malloc(i64)");

        emit_unsafe(malloc->arg(0));
        auto size           = emit(malloc->arg(1));
        auto ptr_t          = convert(Axm::expect<mem::Ptr>(def->proj(1)->type(), "a %mem.Ptr"));
        auto i8ptr          = bb.assign(name + "i8", "call i8* @malloc(i64 {})", size);
        std::string i8ptr_t = "i8*";
        if (Lit::expect(address_space, "an address space") != 0) {
            i8ptr_t = std::format("i8 addrspace({})*", address_space);
            i8ptr   = bb.assign(name + "i8conv", "addrspacecast i8* {} to {}", i8ptr, i8ptr_t);
        }
        return bb.assign(name, "bitcast {} {} to {}", i8ptr_t, i8ptr, ptr_t);
    } else if (auto free = Axm::isa<mem::free>(def)) {
        auto address_space = free->decurry()->arg(1);
        if (Lit::expect(address_space, "an address space") != 0)
            if (auto target_specific = isa_targetspecific_intrinsic(bb, def)) return {};

        declare("void @free(i8*)");
        emit_unsafe(free->arg(0));
        auto ptr   = emit(free->arg(1));
        auto ptr_t = convert(Axm::expect<mem::Ptr>(free->arg(1)->type(), "a %mem.Ptr"));

        auto i8ptr = bb.assign(name + "i8", "bitcast {} {} to i8 addrspace({})*", ptr_t, ptr, address_space);
        if (Lit::expect(address_space, "an address space") != 0)
            i8ptr = bb.assign(name + "i8conv", "addrspacecast i8 addrspace({})* {} to i8*", address_space, i8ptr);
        bb.tail("call void @free(i8* {})", i8ptr);
        return {};
    } else if (auto load = Axm::isa<mem::load>(def)) {
        emit_unsafe(load->arg(0));
        auto v_ptr     = emit(load->arg(1));
        auto t_ptr     = convert(load->arg(1)->type());
        auto t_pointee = convert(Axm::expect<mem::Ptr>(load->arg(1)->type(), "a %mem.Ptr")->arg(0), false);
        return bb.assign(name, "load {}, {} {}", t_pointee, t_ptr, v_ptr);
    } else if (auto store = Axm::isa<mem::store>(def)) {
        emit_unsafe(store->arg(0));
        auto v_ptr = emit(store->arg(1));
        auto v_val = emit(store->arg(2));
        auto t_ptr = convert(store->arg(1)->type());
        auto t_val = convert(store->arg(2)->type(), false);
        std::print(bb.body().emplace_back(), "store {} {}, {} {}", t_val, v_val, t_ptr, v_ptr);
        return {};
    } else if (auto q = Axm::isa<clos::alloc_jmpbuf>(def)) {
        // The size of a `jmp_buf` is platform/libc-dependent, so it is computed by a C runtime
        // wrapper (`rt/mim_rt.c`) rather than hard-coded here; see issue #486.
        declare_rt("i64 @mim_jmpbuf_size()");

        emit_unsafe(q->arg());
        auto size = name + ".size";
        bb.assign(size, "call i64 @mim_jmpbuf_size()");
        return bb.assign(name, "alloca i8, i64 {}", size);
    } else if (auto setjmp = Axm::isa<clos::setjmp>(def)) {
        declare("i32 @_setjmp(i8*) returns_twice");

        auto [mem, jmpbuf] = setjmp->arg()->projs<2>();
        emit_unsafe(mem);
        auto v_jb = emit(jmpbuf);
        return bb.assign(name, "call i32 @_setjmp(i8* {})", v_jb);
    } else if (auto arith = Axm::isa<math::arith>(def)) {
        auto [mode, ab] = arith->uncurry_args<2>();
        auto [a, b]     = ab->projs<2>([this](auto def) { return emit(def); });
        auto t          = convert(arith->type());
        auto lmode      = static_cast<math::Mode>(Lit::expect(mode, "a %math.arith mode"));

        switch (arith.id()) {
            case math::arith::add: op = "fadd"; break;
            case math::arith::sub: op = "fsub"; break;
            case math::arith::mul: op = "fmul"; break;
            case math::arith::div: op = "fdiv"; break;
            case math::arith::rem: op = "frem"; break;
        }

        if (lmode == math::Mode::fast)
            op += " fast";
        else {
            // clang-format off
            if (fe::has_flag(lmode, math::Mode::nnan    )) op += " nnan";
            if (fe::has_flag(lmode, math::Mode::ninf    )) op += " ninf";
            if (fe::has_flag(lmode, math::Mode::nsz     )) op += " nsz";
            if (fe::has_flag(lmode, math::Mode::arcp    )) op += " arcp";
            if (fe::has_flag(lmode, math::Mode::contract)) op += " contract";
            if (fe::has_flag(lmode, math::Mode::afn     )) op += " afn";
            if (fe::has_flag(lmode, math::Mode::reassoc )) op += " reassoc";
            // clang-format on
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto tri = Axm::isa<math::tri>(def)) {
        auto a = emit(tri->arg());
        auto t = convert(tri->type());

        std::string f;

        if (tri.id() == math::tri::sin) {
            f = std::string("llvm.sin") + detail::llvm_suffix(tri->type());
        } else if (tri.id() == math::tri::cos) {
            f = std::string("llvm.cos") + detail::llvm_suffix(tri->type());
        } else {
            if (tri.sub() & sub_t(math::tri::a)) f += "a";

            switch (math::tri((fe::to_underlying(tri.id()) & 0x3) | Annex::base<math::tri>())) {
                case math::tri::sin: f += "sin"; break;
                case math::tri::cos: f += "cos"; break;
                case math::tri::tan: f += "tan"; break;
                case math::tri::ahFF: fe::throwf("this axm is supposed to be unused");
                default: fe::throwf("ll backend: unhandled %math.tri id in '{}'", def);
            }

            if (tri.sub() & sub_t(math::tri::h)) f += "h";
            f += detail::math_suffix(tri->type());
        }

        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto extrema = Axm::isa<math::extrema>(def)) {
        auto [a, b]   = extrema->args<2>([this](auto def) { return emit(def); });
        auto t        = convert(extrema->type());
        std::string f = "llvm.";
        switch (extrema.id()) {
            case math::extrema::fmin: f += "minnum"; break;
            case math::extrema::fmax: f += "maxnum"; break;
            case math::extrema::ieee754min: f += "minimum"; break;
            case math::extrema::ieee754max: f += "maximum"; break;
        }
        f += detail::llvm_suffix(extrema->type());

        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto pow = Axm::isa<math::pow>(def)) {
        auto [a, b]   = pow->args<2>([this](auto def) { return emit(def); });
        auto t        = convert(pow->type());
        std::string f = "llvm.pow";
        f += detail::llvm_suffix(pow->type());
        declare("{} @{}({}, {})", t, f, t, t);
        return bb.assign(name, "tail call {} @{}({} {}, {} {})", t, f, t, a, t, b);
    } else if (auto rt = Axm::isa<math::rt>(def)) {
        auto a = emit(rt->arg());
        auto t = convert(rt->type());
        std::string f;
        if (rt.id() == math::rt::sq)
            f = std::string("llvm.sqrt") + detail::llvm_suffix(rt->type());
        else
            f = std::string("cbrt") += detail::math_suffix(rt->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto exp = Axm::isa<math::exp>(def)) {
        auto a        = emit(exp->arg());
        auto t        = convert(exp->type());
        std::string f = "llvm.";
        f += (exp.sub() & sub_t(math::exp::log)) ? "log" : "exp";
        f += (exp.sub() & sub_t(math::exp::bin)) ? "2" : (exp.sub() & sub_t(math::exp::dec)) ? "10" : "";
        f += detail::llvm_suffix(exp->type());
        // TODO doesn't work for exp10"
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto er = Axm::isa<math::er>(def)) {
        auto a = emit(er->arg());
        auto t = convert(er->type());
        auto f = er.id() == math::er::f ? std::string("erf") : std::string("erfc");
        f += detail::math_suffix(er->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto gamma = Axm::isa<math::gamma>(def)) {
        auto a        = emit(gamma->arg());
        auto t        = convert(gamma->type());
        std::string f = gamma.id() == math::gamma::t ? "tgamma" : "lgamma";
        f += detail::math_suffix(gamma->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto cmp = Axm::isa<math::cmp>(def)) {
        auto [a, b] = cmp->args<2>([this](auto def) { return emit(def); });
        auto t      = convert(cmp->arg(0)->type());
        op          = "fcmp ";

        switch (cmp.id()) {
                // clang-format off
            case math::cmp::  e: op += "oeq"; break;
            case math::cmp::  l: op += "olt"; break;
            case math::cmp:: le: op += "ole"; break;
            case math::cmp::  g: op += "ogt"; break;
            case math::cmp:: ge: op += "oge"; break;
            case math::cmp:: ne: op += "one"; break;
            case math::cmp::  o: op += "ord"; break;
            case math::cmp::  u: op += "uno"; break;
            case math::cmp:: ue: op += "ueq"; break;
            case math::cmp:: ul: op += "ult"; break;
            case math::cmp::ule: op += "ule"; break;
            case math::cmp:: ug: op += "ugt"; break;
            case math::cmp::uge: op += "uge"; break;
            case math::cmp::une: op += "une"; break;
            // clang-format on
            default: fe::throwf("ll backend: unhandled %math.cmp id in '{}'", def);
        }

        return bb.assign(name, "{} {} {}, {}", op, t, a, b);
    } else if (auto is_finite = Axm::isa<math::is_finite>(def)) {
        // https://llvm.org/docs/LangRef.html#llvm-is-fpclass-intrinsic
        // declare i1 @llvm.is.fpclass(<fptype> <op>, i32 <test>)
        auto a  = emit(is_finite->arg());
        auto at = convert(is_finite->arg()->type());
        auto t  = convert(is_finite->type());

        auto s = detail::llvm_suffix(is_finite->arg()->type());
        auto f = "llvm.is.fpclass";
        declare("{} @{}{}({}, i32)", t, f, s, at);
        return bb.assign(name, "tail call {} @{}{}({} {}, i32 504)", t, f, s, at, a);
    } else if (auto conv = Axm::isa<math::conv>(def)) {
        auto v_src = emit(conv->arg());
        auto t_src = convert(conv->arg()->type());
        auto t_dst = convert(conv->type());

        auto s_src = math::isa_f(conv->arg()->type());
        auto s_dst = math::isa_f(conv->type());

        switch (conv.id()) {
            case math::conv::f2f: op = s_src < s_dst ? "fpext" : "fptrunc"; break;
            case math::conv::s2f: op = "sitofp"; break;
            case math::conv::u2f: op = "uitofp"; break;
            case math::conv::f2s: op = "fptosi"; break;
            case math::conv::f2u: op = "fptoui"; break;
        }

        return bb.assign(name, "{} {} {} to {}", op, t_src, v_src, t_dst);
    } else if (auto abs = Axm::isa<math::abs>(def)) {
        auto a        = emit(abs->arg());
        auto t        = convert(abs->type());
        std::string f = "llvm.fabs";
        f += detail::llvm_suffix(abs->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto round = Axm::isa<math::round>(def)) {
        auto a        = emit(round->arg());
        auto t        = convert(round->type());
        std::string f = "llvm.";
        switch (round.id()) {
            case math::round::f: f += "floor"; break;
            case math::round::c: f += "ceil"; break;
            case math::round::r: f += "round"; break;
            case math::round::t: f += "trunc"; break;
        }
        f += detail::llvm_suffix(round->type());
        declare("{} @{}({})", t, f, t);
        return bb.assign(name, "tail call {} @{}({} {})", t, f, t, a);
    } else if (auto zip = Axm::isa<vec::zip>(def)) {
        auto ni_n   = zip->decurry()->decurry()->decurry()->arg();
        auto nat_ni = Lit::expect(ni_n->proj(2, 0), "a %vec.zip lane count");
        auto f      = zip->decurry()->arg();
        auto inputs = zip->arg();
        auto t_in   = convert(inputs->proj(nat_ni, 0)->type());
        auto t_out  = convert(def->type()); // <n x T>

        std::string op;
        std::string prev;

        if (auto nat_op = Axm::isa<core::nat, 1>(f)) {
            switch (nat_op.id()) {
                case core::nat::add: op = "add nuw nsw"; break;
                case core::nat::sub: {
                    // nat subtraction saturates at 0: cap per-lane when v2 > v1
                    auto v1     = emit(inputs->proj(nat_ni, 0));
                    auto v2     = emit(inputs->proj(nat_ni, 1));
                    auto ugt    = bb.assign(name + ".ugt", "icmp ugt {} {}, {}", t_in, v2, v1);
                    auto raw    = bb.assign(name + ".raw", "sub {} {}, {}", t_in, v1, v2);
                    return prev = bb.assign(name, "select <{} x i1> {}, {} zeroinitializer, {} {}", nat_ni, ugt, t_out,
                                            t_out, raw);
                }
                case core::nat::mul: op = "mul nuw nsw"; break;
                case core::nat::div: op = "udiv"; break;
                case core::nat::mod: op = "urem"; break;
            }
        } else if (auto arith_op = Axm::isa<math::arith, 1>(f)) {
            auto lmode = static_cast<math::Mode>(
                Lit::expect(f->expect<App>("a zipped %math.arith")->arg(), "a %math.arith mode"));
            switch (arith_op.id()) {
                case math::arith::add: op = "fadd"; break;
                case math::arith::sub: op = "fsub"; break;
                case math::arith::mul: op = "fmul"; break;
                case math::arith::div: op = "fdiv"; break;
                case math::arith::rem: op = "frem"; break;
            }

            if (lmode == math::Mode::fast)
                op += " fast";
            else {
                if (fe::has_flag(lmode, math::Mode::nnan)) op += " nnan";
                if (fe::has_flag(lmode, math::Mode::ninf)) op += " ninf";
                if (fe::has_flag(lmode, math::Mode::nsz)) op += " nsz";
                if (fe::has_flag(lmode, math::Mode::arcp)) op += " arcp";
                if (fe::has_flag(lmode, math::Mode::contract)) op += " contract";
                if (fe::has_flag(lmode, math::Mode::afn)) op += " afn";
                if (fe::has_flag(lmode, math::Mode::reassoc)) op += " reassoc";
            }
        } else if (auto ncmp_op = Axm::isa<core::ncmp, 1>(f)) {
            op = "icmp ";
            switch (ncmp_op.id()) {
                case core::ncmp::e: op += "eq"; break;
                case core::ncmp::ne: op += "ne"; break;
                case core::ncmp::g: op += "ugt"; break;
                case core::ncmp::ge: op += "uge"; break;
                case core::ncmp::l: op += "ult"; break;
                case core::ncmp::le: op += "ule"; break;
                default: fe::throwf("ll backend: unhandled zipped %core.ncmp id in '{}'", def);
            }
        } else if (auto icmp_op = Axm::isa<core::icmp, 1>(f)) {
            op = "icmp ";
            switch (icmp_op.id()) {
                case core::icmp::e: op += "eq"; break;
                case core::icmp::ne: op += "ne"; break;
                case core::icmp::sg: op += "sgt"; break;
                case core::icmp::sge: op += "sge"; break;
                case core::icmp::sl: op += "slt"; break;
                case core::icmp::sle: op += "sle"; break;
                case core::icmp::ug: op += "ugt"; break;
                case core::icmp::uge: op += "uge"; break;
                case core::icmp::ul: op += "ult"; break;
                case core::icmp::ule: op += "ule"; break;
                default: fe::throwf("ll backend: unhandled zipped %core.icmp id in '{}'", def);
            }
        } else if (auto mcmp_op = Axm::isa<math::cmp, 1>(f)) {
            op = "fcmp ";
            switch (mcmp_op.id()) {
                case math::cmp::e: op += "oeq"; break;
                case math::cmp::l: op += "olt"; break;
                case math::cmp::le: op += "ole"; break;
                case math::cmp::g: op += "ogt"; break;
                case math::cmp::ge: op += "oge"; break;
                case math::cmp::ne: op += "one"; break;
                case math::cmp::o: op += "ord"; break;
                case math::cmp::u: op += "uno"; break;
                case math::cmp::ue: op += "ueq"; break;
                case math::cmp::ul: op += "ult"; break;
                case math::cmp::ule: op += "ule"; break;
                case math::cmp::ug: op += "ugt"; break;
                case math::cmp::uge: op += "uge"; break;
                case math::cmp::une: op += "une"; break;
                default: fe::throwf("ll backend: unhandled zipped %math.cmp id in '{}'", def);
            }
        } else {
            fe::throwf("unhandled vec.zip operation: {}", f);
        }

        auto v1 = emit(inputs->proj(nat_ni, 0));
        auto v2 = emit(inputs->proj(nat_ni, 1));
        prev    = bb.assign(name, "{} {} {}, {}", op, t_in, v1, v2);
        return prev;
    } else if (auto res = isa_targetspecific_intrinsic(bb, def)) {
        return res.value();
    }
    fe::throwf("unhandled def in LLVM backend: {} : {}", def, def->type());
}

extern "C" {
MIM_EXPORT void mim_ll_convert(Emitter& e, const Def* type, bool simd, std::string& res) {
    res = e.convert_impl(type, simd);
}
MIM_EXPORT void mim_ll_finalize(Emitter& e) { e.finalize_impl(); }
MIM_EXPORT void mim_ll_emit_epilogue(Emitter& e, Lam* lam) { e.emit_epilogue_impl(lam); }
MIM_EXPORT void mim_ll_emit_bb(Emitter& e, BB& bb, const Def* def, std::string& res) { res = e.emit_bb_impl(bb, def); }
}

} // namespace mim::plug::ll

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::ll::emit, plug::ll::Emit>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"ll", MIM_VERSION, nullptr, reg_phases}; }
