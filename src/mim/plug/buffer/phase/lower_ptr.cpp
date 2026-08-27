#include "mim/plug/buffer/phase/lower_ptr.h"

#include <mim/axm.h>
#include <mim/def.h>

#include <mim/plug/mem/mem.h>

#include "mim/plug/buffer/buffer.h"

namespace mim::plug::buffer {

namespace {

/// Successively offsets `ptr` by each component of the index `tuple`, peeling one array dimension per `%mem.lea`.
const Def* op_lea_tuple(const Def* ptr, const Def* tuple) {
    auto n       = tuple->num_projs();
    auto element = ptr;
    for (size_t i = 0; i < n; ++i)
        element = mem::op_lea(element, tuple->proj(n, i));
    return element;
}

/// Builds the nested array type `«s; T»` from a shape tuple `s` and element type `T`.
const Def* arr_ty_of(const Def* s, const Def* T) {
    auto& w     = s->world();
    auto n      = s->num_projs();
    auto arr_ty = T;
    for (int i = (int)n - 1; i >= 0; --i)
        arr_ty = w.arr(s->proj(n, i), arr_ty);
    return arr_ty;
}

/// Builds the nested `pack` `‹s; val›` replicating `val` across every element.
const Def* pack_tuple(const Def* s, const Def* val) {
    auto& w      = val->world();
    auto n       = s->num_projs();
    auto element = val;
    for (int i = (int)n - 1; i >= 0; --i)
        element = w.pack(s->proj(n, i), element);
    return element;
}

} // namespace

const Def* LowerPtr::rewrite_imm_App(const App* app) {
    if (is_bootstrapping()) return RWPhase::rewrite_imm_App(app);
    auto& w = new_world();

    if (auto buf_ax = Axm::isa<buffer::Buf>(app)) {
        auto [r, s, T] = buf_ax->args<3>();
        s              = rewrite(s);
        T              = rewrite(T);
        return w.call<mem::Ptr>(Defs{arr_ty_of(s, T), w.lit_nat_0()});
    } else if (auto alloc_ax = Axm::isa<buffer::alloc>(app)) {
        auto mem         = rewrite(alloc_ax->arg());
        auto [r, s, T]   = alloc_ax->callee()->as<App>()->args<3>();
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(arr_ty_of(s, T), mem)->projs<2>();
        return w.tuple({mem2, ptr});
    } else if (auto read_ax = Axm::isa<buffer::read>(app)) {
        auto [mem, buf, idx] = read_ax->args<3>();
        mem                  = rewrite(mem);
        buf                  = rewrite(buf);
        idx                  = rewrite(idx);
        auto element_ptr     = op_lea_tuple(buf, idx);
        auto [mem2, val]     = w.call<mem::load>(Defs{mem, element_ptr})->projs<2>();
        return w.tuple({mem2, val});
    } else if (auto write_ax = Axm::isa<buffer::write>(app)) {
        auto [mem, buf, idx, val] = write_ax->args<4>();
        mem                       = rewrite(mem);
        buf                       = rewrite(buf);
        idx                       = rewrite(idx);
        val                       = rewrite(val);
        auto element_ptr          = op_lea_tuple(buf, idx);
        auto mem2                 = w.call<mem::store>(Defs{mem, element_ptr, val});
        return w.tuple({mem2, buf});
    } else if (auto copy_ax = Axm::isa<buffer::copy>(app)) {
        auto [mem, dst, src] = copy_ax->args<3>();
        mem                  = rewrite(mem);
        dst                  = rewrite(dst);
        src                  = rewrite(src);
        // Whole-buffer copy: load the entire array out of `src` and store it into `dst`.
        auto [mem2, val] = w.call<mem::load>(Defs{mem, src})->projs<2>();
        return w.call<mem::store>(Defs{mem2, dst, val});
    } else if (auto init_ax = Axm::isa<buffer::init>(app)) {
        auto [mem, val]  = init_ax->args<2>();
        auto [r, s, T]   = init_ax->callee()->as<App>()->args<3>();
        mem              = rewrite(mem);
        val              = rewrite(val);
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(arr_ty_of(s, T), mem)->projs<2>();
        auto mem3        = w.call<mem::store>(Defs{mem2, ptr, val});
        return w.tuple({mem3, ptr});
    } else if (auto const_ax = Axm::isa<buffer::lit>(app)) {
        auto [mem, val]  = const_ax->args<2>();
        auto [r, s, T]   = const_ax->callee()->as<App>()->args<3>();
        mem              = rewrite(mem);
        val              = rewrite(val);
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(arr_ty_of(s, T), mem)->projs<2>();
        auto mem3        = w.call<mem::store>(Defs{mem2, ptr, pack_tuple(s, val)});
        return w.tuple({mem3, ptr});
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::buffer
