#include "mim/plug/buffer/phase/lower_ptr.h"

#include <mim/axm.h>
#include <mim/def.h>

#include <mim/plug/gpu/gpu.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/buffer/buffer.h"

namespace mim::plug::buffer {

const Def* LowerPtr::rewrite_imm_App(const App* app) {
    auto& w = new_world();

    if (auto buf_ax = Axm::isa<buffer::Buf>(app)) {
        auto [r, s, T] = buf_ax->args<3>();
        s              = rewrite(s);
        T              = rewrite(T);
        return w.call<mem::Ptr>(Defs{w.arr(s, T), w.lit_nat_0()});
    } else if (auto alloc_ax = Axm::isa<buffer::alloc>(app)) {
        auto mem         = rewrite(alloc_ax->arg());
        auto [r, s, T]   = alloc_ax->callee()->as<App>()->args<3>();
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(w.arr(s, T), mem)->projs<2>();
        return w.tuple({mem2, ptr});
    } else if (auto read_ax = Axm::isa<buffer::read>(app)) {
        auto [mem, buf, idx] = read_ax->args<3>();
        mem                  = rewrite(mem);
        buf                  = rewrite(buf);
        idx                  = rewrite(idx);
        auto element_ptr     = mem::op_lea_tuple(buf, idx);
        auto [mem2, val]     = w.call<mem::load>(Defs{mem, element_ptr})->projs<2>();
        return w.tuple({mem2, val});
    } else if (auto write_ax = Axm::isa<buffer::write>(app)) {
        auto [mem, buf, idx, val] = write_ax->args<4>();
        mem                       = rewrite(mem);
        buf                       = rewrite(buf);
        idx                       = rewrite(idx);
        val                       = rewrite(val);
        auto element_ptr          = mem::op_lea_tuple(buf, idx);
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
    } else if (auto free_ax = Axm::isa<buffer::free>(app)) {
        auto [mem, buf] = free_ax->args<2>([this](const Def* d) { return rewrite(d); });
        return w.call<mem::free>(Defs{mem, buf});
    } else if (auto init_ax = Axm::isa<buffer::init>(app)) {
        auto [mem, val]  = init_ax->args<2>();
        auto [r, s, T]   = init_ax->callee()->as<App>()->args<3>();
        mem              = rewrite(mem);
        val              = rewrite(val);
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(w.arr(s, T), mem)->projs<2>();
        auto mem3        = w.call<mem::store>(Defs{mem2, ptr, val});
        return w.tuple({mem3, ptr});
    } else if (auto const_ax = Axm::isa<buffer::lit>(app)) {
        auto [mem, val]  = const_ax->args<2>();
        auto [r, s, T]   = const_ax->callee()->as<App>()->args<3>();
        mem              = rewrite(mem);
        val              = rewrite(val);
        s                = rewrite(s);
        T                = rewrite(T);
        auto [mem2, ptr] = mem::op_alloc(w.arr(s, T), mem)->projs<2>();
        auto mem3        = w.call<mem::store>(Defs{mem2, ptr, w.pack(s, val)});
        return w.tuple({mem3, ptr});
    } else if (auto buf_alloc_copy = Axm::isa<gpu::buf_alloc_copy>(app)) {
        auto [m0, m1, ptr] = buf_alloc_copy->args<3>([this](const Def* d) { return rewrite(d); });
        return w.call(gpu::alloc_copy::block, Defs{m0, m1, ptr});
    } else if (auto buf_copy_to_host = Axm::isa<gpu::buf_copy_to_host>(app)) {
        auto [m0, m1, d_ptr, h_ptr] = buf_copy_to_host->args<4>([this](const Def* d) { return rewrite(d); });
        return w.call(gpu::copy_to_host::block, Defs{m0, m1, d_ptr, h_ptr});
    }

    return RWPhase::rewrite_imm_App(app);
}

} // namespace mim::plug::buffer
