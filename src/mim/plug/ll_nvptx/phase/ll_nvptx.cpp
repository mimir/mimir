#include "mim/plug/ll_nvptx/phase/ll_nvptx.h"

#include <format>

#include <mim/driver.h>

#include <mim/util/sys.h>

#include <mim/plug/core/core.h>
#include <mim/plug/gpu/gpu.h>
#include <mim/plug/mem/mem.h>

#include "mim/plug/ll_nvptx/ll_nvptx.h"

using namespace std::string_literals;

namespace mim::plug::ll_nvptx {

namespace core = mim::plug::core;
namespace ll   = mim::plug::ll;
namespace mem  = mim::plug::mem;
namespace gpu  = mim::plug::gpu;

class HostEmitter : public ll::Emitter {
public:
    using Super = ll::Emitter;

    HostEmitter(World& world, std::ostream& ostream, std::optional<std::string> device_fatbin_file)
        : Super(world, "llvm_nvptx_host_emitter", ostream)
        , device_fatbin_file_(device_fatbin_file) {}

    void start() final;
    void find_kernels(const Def*);

    std::optional<std::string> isa_targetspecific_intrinsic(ll::BB&, const Def*) final;

protected:
    std::string convert(const Def*, bool simd = true) override;

private:
    static constexpr std::string_view mod_name_          = "@.mimir_cu_mod";
    static constexpr std::string_view ctx_name_          = "@.mimir_cu_ctx";
    static constexpr std::string_view fatbin_name_       = "@.fatbin";
    static constexpr std::string_view kernel_array_name_ = "@.mimir_kernels";
    static constexpr std::string_view kernel_name_prefix = "@.kname.";

    void emit_cu_error_handling(ll::BB&, const std::string&);

    std::optional<std::string> device_fatbin_file_;
    LamMap<int> kernel_ids_;
    bool cu_globals_declared_ = false;

    DefSet analyzed_;
};

class DeviceEmitter : public ll::Emitter {
public:
    using Super = ll::Emitter;

    DeviceEmitter(World& world, std::ostream& ostream)
        : Super(world, "llvm_nvptx_device_emitter", ostream) {}

    void start() final;

    std::string prepare() override;

    std::optional<std::string> isa_targetspecific_intrinsic(ll::BB&, const Def*) final;

    bool is_using_libdevice() const { return uses_libdevice; }
    const std::string& get_extra_flags() const { return extra_flags; }

private:
    std::string convert(const Def* def, bool simd = false) override {
        if (simd) WLOG("Ignoring simd=true for type conversion in device code.");
        return Super::convert(def, false);
    }

    /// Device slots live in a module-scope global in their requested address space, not on the stack.
    std::string emit_slot(ll::BB&, const App* app, const Def* pointee, const Def* addr_space) override {
        auto v_ptr = "@" + app->unique_name() + ".slot";
        std::print(vars_decls_, "{} = internal addrspace({}) global {} undef\n", v_ptr, addr_space, convert(pointee));
        return v_ptr;
    }

    absl::btree_map<std::string, int> symbols_;
    LamSet kernels_;

    bool uses_libdevice;
    std::string extra_flags;
};

void HostEmitter::start() {
    for (auto def : world().annexes().defs())
        find_kernels(def);
    for (auto def : world().externals().muts())
        find_kernels(def);

    for (auto [kernel, kid] : kernel_ids_) {
        auto name = id(kernel).substr(1);
        std::print(vars_decls_, "{}{} = private constant [{} x i8] c\"{}\\00\"\n", kernel_name_prefix, kid,
                   name.size() + 1, name);
    }
    std::print(vars_decls_, "{} = dso_local global [{} x ptr] zeroinitializer\n", kernel_array_name_,
               kernel_ids_.size());

    Super::start();
}

void HostEmitter::find_kernels(const Def* def) {
    if (auto [_, ins] = analyzed_.emplace(def); !ins) return;

    for (auto d : def->deps())
        find_kernels(d);

    if (auto launch = Axm::isa<gpu::launch>(def)) {
        auto kernel     = launch->decurry()->decurry()->arg();
        auto kernel_lam = kernel->expect_mut<Lam>("the kernel passed to `%gpu.launch` to be a mutable lambda");
        if (kernel_ids_.contains(kernel_lam)) return;
        auto kid                = kernel_ids_.size();
        kernel_ids_[kernel_lam] = kid;
    }
}

constexpr auto Cu_Init                = "cuInit";
constexpr auto Cu_Ctx_Create          = "cuCtxCreate_v4";
constexpr auto Cu_Ctx_Destroy         = "cuCtxDestroy_v2";
constexpr auto Cu_Device_Get          = "cuDeviceGet";
constexpr auto Cu_Launch_Kernel       = "cuLaunchKernel_ptsz";
constexpr auto Cu_Mem_Alloc           = "cuMemAlloc_v2";
constexpr auto Cu_Mem_Alloc_Async     = "cuMemAllocAsync_ptsz";
constexpr auto Cu_Mem_Free            = "cuMemFree_v2";
constexpr auto Cu_Mem_Free_Async      = "cuMemFreeAsync_ptsz";
constexpr auto Cu_Memcpy_Htod         = "cuMemcpyHtoD_v2";
constexpr auto Cu_Memcpy_Htod_Async   = "cuMemcpyHtoDAsync_v2_ptsz";
constexpr auto Cu_Memcpy_Dtoh         = "cuMemcpyDtoH_v2";
constexpr auto Cu_Memcpy_Dtoh_Async   = "cuMemcpyDtoHAsync_v2_ptsz";
constexpr auto Cu_Module_Load_Fatbin  = "cuModuleLoadFatBinary";
constexpr auto Cu_Module_Get_Function = "cuModuleGetFunction";
constexpr auto Cu_Module_Unload       = "cuModuleUnload";
constexpr auto Cu_Stream_Create       = "cuStreamCreate";
constexpr auto Cu_Stream_Destroy      = "cuStreamDestroy_v2";
constexpr auto Cu_Stream_Sync         = "cuStreamSynchronize_ptsz";

void HostEmitter::emit_cu_error_handling(ll::BB& bb, const std::string& cu_result) {
    // Offload the CUresult check to the C runtime wrapper `mim_cu_check` (see rt/mim_cuda_rt.c)
    // instead of open-coding it here; showcases the C-runtime system on the ll_nvptx backend.
    declare_rt("void @mim_cu_check(i32)");
    std::print(bb.body().emplace_back(), "call void @mim_cu_check(i32 {})", cu_result);
}

std::string HostEmitter::convert(const Def* type, bool simd) {
    if (auto ptr = Axm::isa<mem::Ptr>(type)) {
        auto [_, addr_space] = ptr->args<2>();
        auto lit             = Lit::isa(addr_space);
        if (lit.value_or(0L) != 0) {
            // NVIDIA treats all device pointers as i64s in host code
            return "i64";
        }
    }
    return Super::convert(type, simd);
}

std::optional<std::string> HostEmitter::isa_targetspecific_intrinsic(ll::BB& bb, const Def* def) {
    auto name = id(def);
    std::string op;

    if (auto default_stream = Axm::isa<gpu::default_stream>(def)) {
        return "null";
    } else if (auto init = Axm::isa<gpu::init>(def)) {
        auto mem_val = emit_unsafe(init->arg());

        auto dev_num   = 0; // TODO: consider parameterizing this
        auto ctx_flags = 0; // TODO: consider parameterizing this

        declare("i32 @{}(i32)", Cu_Init);
        auto init_res = bb.assign(name + "_init_res", "call i32 @{}(i32 0)", Cu_Init);
        emit_cu_error_handling(bb, init_res);

        declare("i32 @{}(ptr, i32)", Cu_Device_Get);
        auto dev_ptr = bb.assign(name + "_dev_ptr", "alloca i32");
        auto dev_get_res
            = bb.assign(name + "_get_res", "call i32 @{}(ptr {}, i32 {})", Cu_Device_Get, dev_ptr, dev_num);
        emit_cu_error_handling(bb, dev_get_res);

        declare("i32 @{}(ptr, ptr, i32, i32)", Cu_Ctx_Create);
        if (!cu_globals_declared_) std::print(vars_decls_, "{} = global ptr null\n", ctx_name_);
        auto dev     = bb.assign(name + "_dev", "load i32, ptr {}", dev_ptr);
        auto ctx_res = bb.assign(name + "_ctx_res", "call i32 @{}(ptr {}, ptr null, i32 {}, i32 {})", Cu_Ctx_Create,
                                 ctx_name_, ctx_flags, dev);
        emit_cu_error_handling(bb, ctx_res);

        declare("i32 @{}(ptr, ptr)", Cu_Module_Load_Fatbin);
        if (!cu_globals_declared_) {
            std::print(vars_decls_, "{} = global ptr null\n", mod_name_);
            if (device_fatbin_file_.has_value()) {
                std::ifstream fatbin_file(device_fatbin_file_.value(), std::ios::binary);
                if (!fatbin_file)
                    fe::throwf(MIM_LL_NVPTX_BE "could not open `{}` as binary file", device_fatbin_file_.value());

                auto start = std::istreambuf_iterator<char>(fatbin_file);
                auto end   = std::istreambuf_iterator<char>();
                std::vector<u8> fatbin_bytes(start, end);

                std::print(vars_decls_, "{} = private constant [{} x i8] c\"", fatbin_name_, fatbin_bytes.size());
                for (auto byte : fatbin_bytes) {
                    bool invalid_cstr_char = byte == '"' || byte == '\\';
                    if (std::isprint(byte) && !invalid_cstr_char) {
                        std::print(vars_decls_, "{:c}", byte);
                    } else {
                        auto byte_val = static_cast<int>(byte);
                        std::print(vars_decls_, "\\{:x}{:x}", byte_val / 16, byte_val % 16);
                    }
                }
                std::print(vars_decls_, "\"\n");
            } else {
                std::print(vars_decls_, "; Add the bytes of your compiled nvptx fatbin binary here:\n");
                std::print(vars_decls_,
                           "{} = private constant [YOUR_FATBIN_DATA_SIZE_GOES_HERE x i8] YOUR_FATBIN_DATA_GOES_HERE\n",
                           fatbin_name_);
            }
            cu_globals_declared_ = true;
        }
        auto mod_res = bb.assign(name + "_mod_res", "call i32 @{}(ptr {}, ptr {})", Cu_Module_Load_Fatbin, mod_name_,
                                 fatbin_name_);
        emit_cu_error_handling(bb, mod_res);
        auto mod_inner = bb.assign(name + "_mod_inner", "load ptr, ptr {}", mod_name_);

        declare("i32 @{}(ptr, ptr, ptr)", Cu_Module_Get_Function);
        for (auto [kernel, kid] : kernel_ids_) {
            auto kname    = id(kernel).substr(1);
            auto func_ptr = bb.assign(name + "_" + kname + "_funcptr", "getelementptr inbounds ptr, ptr {}, i64 {}",
                                      kernel_array_name_, kid);
            auto func_res = bb.assign(name + "_" + kname + "_getfuncres", "call i32 @{}(ptr {}, ptr {}, ptr {}{})",
                                      Cu_Module_Get_Function, func_ptr, mod_inner, kernel_name_prefix, kid);
            emit_cu_error_handling(bb, func_res);
        }

        return mem_val;
    } else if (auto deinit = Axm::isa<gpu::deinit>(def)) {
        emit_unsafe(deinit->arg(0));
        emit_unsafe(deinit->arg(1));

        declare("i32 @{}(ptr)", Cu_Module_Unload);
        std::print(bb.body().emplace_back(), "{}_mod = load ptr, ptr {}", name, mod_name_);
        std::print(bb.body().emplace_back(), "{}_mod_unload_res = call i32 @{}(ptr {}_mod)", name, Cu_Module_Unload,
                   name);
        emit_cu_error_handling(bb, name + "_mod_unload_res");

        declare("i32 @{}(ptr)", Cu_Ctx_Destroy);
        std::print(bb.body().emplace_back(), "{}_ctx = load ptr, ptr {}", name, ctx_name_);
        std::print(bb.body().emplace_back(), "{}_ctx_destroy_res = call i32 @{}(ptr {}_ctx)", name, Cu_Ctx_Destroy,
                   name);
        emit_cu_error_handling(bb, name + "_ctx_destroy_res");

        return ""s;
    } else if (auto stream_init = Axm::isa<gpu::stream_init>(def)) {
        declare("i32 @{}(ptr, i32)", Cu_Stream_Create);

        emit_unsafe(stream_init->arg(0));
        emit_unsafe(stream_init->arg(1));
        auto stream_ptr = emit(stream_init->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {}, i32 0)", Cu_Stream_Create, stream_ptr);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto stream_deinit = Axm::isa<gpu::stream_deinit>(def)) {
        declare("i32 @{}(ptr)", Cu_Stream_Destroy);

        emit_unsafe(stream_deinit->arg(0));
        emit_unsafe(stream_deinit->arg(1));
        auto stream = emit(stream_deinit->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {})", Cu_Stream_Destroy, stream);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto stream_sync = Axm::isa<gpu::stream_sync>(def)) {
        declare("i32 @{}(ptr)", Cu_Stream_Sync);

        emit_unsafe(stream_sync->arg(0));
        emit_unsafe(stream_sync->arg(1));
        auto stream = emit(stream_sync->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {})", Cu_Stream_Sync, stream);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto alloc = Axm::isa<gpu::alloc>(def)) {
        bool is_async;
        switch (alloc.id()) {
            case gpu::alloc::block: is_async = false; break;
            case gpu::alloc::asyn: is_async = true; break;
            default: fe::throwf(MIM_LL_NVPTX_BE "unhandled `%gpu.alloc` id in `{}`", def);
        }

        if (is_async)
            declare("i32 @{}(ptr, i64, ptr)", Cu_Mem_Alloc_Async);
        else
            declare("i32 @{}(ptr, i64)", Cu_Mem_Alloc);

        emit_unsafe(alloc->arg(0));
        auto alloc_t    = alloc->decurry()->arg();
        World& w        = alloc_t->world();
        auto type_size  = w.call(core::trait::size, alloc_t);
        auto alloc_size = emit(type_size);

        auto ptr_t = convert(Axm::expect<mem::Ptr>(def->proj(1)->type(), "a `%mem.Ptr`"));

        auto alloc_ptr = bb.assign(name + "ptr", "alloca {}", ptr_t);
        std::string alloc_res;
        if (is_async) {
            auto stream = emit(alloc->arg(1));
            alloc_res   = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, ptr {})", Cu_Mem_Alloc_Async, alloc_ptr,
                                    alloc_size, stream);
        } else
            alloc_res = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {})", Cu_Mem_Alloc, alloc_ptr, alloc_size);

        emit_cu_error_handling(bb, alloc_res);
        return bb.assign(name, "load {}, {} addrspace(0)* {}", ptr_t, ptr_t, alloc_ptr);
    } else if (auto free = Axm::isa<gpu::free>(def)) {
        bool is_async;
        switch (free.id()) {
            case gpu::free::block: is_async = false; break;
            case gpu::free::asyn: is_async = true; break;
            default: fe::throwf(MIM_LL_NVPTX_BE "unhandled `%gpu.free` id in `{}`", def);
        }

        if (is_async)
            declare("i32 @{}(i64)", Cu_Mem_Free_Async);
        else
            declare("i32 @{}(i64)", Cu_Mem_Free);

        emit_unsafe(free->arg(0));
        auto ptr = emit(free->arg(1));

        std::string free_res;
        if (is_async) {
            auto stream = emit(free->arg(2));
            free_res    = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {})", Cu_Mem_Free_Async, ptr, stream);
        } else
            free_res = bb.assign(name + "res", "call i32 @{}(i64 {})", Cu_Mem_Free, ptr);

        emit_cu_error_handling(bb, free_res);
        return free_res;
    } else if (auto copy_to_device = Axm::isa<gpu::copy_to_device>(def)) {
        bool is_async;
        switch (copy_to_device.id()) {
            case gpu::copy_to_device::block: is_async = false; break;
            case gpu::copy_to_device::asyn: is_async = true; break;
            default: fe::throwf(MIM_LL_NVPTX_BE "unhandled `%gpu.copy_to_device` id in `{}`", def);
        }

        if (is_async)
            declare("i32 @{}(i64, ptr, i64, ptr)", Cu_Memcpy_Htod_Async);
        else
            declare("i32 @{}(i64, ptr, i64)", Cu_Memcpy_Htod);

        auto type      = copy_to_device->decurry()->arg();
        World& w       = type->world();
        auto type_size = w.call(core::trait::size, type);

        emit_unsafe(copy_to_device->arg(0));
        emit_unsafe(copy_to_device->arg(1));
        auto host_ptr = emit(copy_to_device->arg(2));
        auto dev_ptr  = emit(copy_to_device->arg(3));
        auto size     = emit(type_size);

        std::string copy_res;
        if (is_async) {
            auto stream = emit(copy_to_device->arg(4));
            copy_res    = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {}, i64 {}, ptr {})", Cu_Memcpy_Htod_Async,
                                    dev_ptr, host_ptr, size, stream);
        } else
            copy_res = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {}, i64 {})", Cu_Memcpy_Htod, dev_ptr,
                                 host_ptr, size);

        emit_cu_error_handling(bb, copy_res);
        return copy_res;
    } else if (auto copy_to_host = Axm::isa<gpu::copy_to_host>(def)) {
        bool is_async;
        switch (copy_to_host.id()) {
            case gpu::copy_to_host::block: is_async = false; break;
            case gpu::copy_to_host::asyn: is_async = true; break;
            default: fe::throwf(MIM_LL_NVPTX_BE "unhandled `%gpu.copy_to_host` id in `{}`", def);
        }
        if (is_async)
            declare("i32 @{}(ptr, i64, i64, ptr)", Cu_Memcpy_Dtoh_Async);
        else
            declare("i32 @{}(ptr, i64, i64)", Cu_Memcpy_Dtoh);

        auto [type]    = copy_to_host->decurry()->args<1>();
        World& w       = type->world();
        auto type_size = w.call(core::trait::size, type);

        emit_unsafe(copy_to_host->arg(0));
        emit_unsafe(copy_to_host->arg(1));
        auto dev_ptr  = emit(copy_to_host->arg(2));
        auto host_ptr = emit(copy_to_host->arg(3));
        auto size     = emit(type_size);

        std::string copy_res;
        if (is_async) {
            auto stream = emit(copy_to_host->arg(4));
            copy_res    = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, i64 {}, ptr {})", Cu_Memcpy_Dtoh_Async,
                                    host_ptr, dev_ptr, size, stream);
        } else
            copy_res = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, i64 {})", Cu_Memcpy_Dtoh, host_ptr,
                                 dev_ptr, size);

        emit_cu_error_handling(bb, copy_res);
        return copy_res;
    } else if (auto launch = Axm::isa<gpu::launch>(def)) {
        // TODO: rewrite to use modern cuLaunchKernelEx instead
        declare("i32 @{}(ptr, i32, i32, i32, i32, i32, i32, i32, ptr, ptr, ptr)", Cu_Launch_Kernel);

        auto [implicits, launch_config, kernel_def, arg_def, func_args] = launch->uncurry_args<5>();
        auto [n_groups_def, n_items_def, stream_def, m, MT]             = launch_config->projs<5>();
        auto [mem, ret_lam_def]                                         = func_args->projs<2>();

        Lam* lam = kernel_def->isa_mut<Lam>();
        if (!lam) fe::throwf(MIM_LL_NVPTX_BE "kernel `{}` is not a lambda", kernel_def);
        if (!kernel_ids_.contains(lam)) fe::throwf(MIM_LL_NVPTX_BE "unknown kernel `{}`", lam);
        auto kid = kernel_ids_[lam];

        auto shared_mem_bytes = 0;
        if (auto smem_count = Lit::expect(m, "a shared-memory allocation count")) {
            if (smem_count != 1)
                fe::throwf(MIM_LL_NVPTX_BE "only one dynamic shared-memory allocation is allowed per kernel");
            shared_mem_bytes = Lit::expect(world().call(core::trait::size, MT), "a shared-memory size");
        }

        emit_unsafe(mem);
        auto n_groups = emit(n_groups_def);
        auto n_items  = emit(n_items_def);
        auto stream   = emit(stream_def);
        auto kernel   = emit(kernel_def);
        auto arg      = emit(arg_def);
        auto arg_type = convert(arg_def->type());
        auto ret_lam  = emit(ret_lam_def);

        auto func_ptr = bb.assign(name + "_kernptr", "getelementptr inbounds [{} x ptr], [{} x ptr]* {}, i64 0, i64 {}",
                                  kernel_ids_.size(), kernel_ids_.size(), kernel_array_name_, kid);
        auto func_inner = bb.assign(name + "_kernel", "load ptr, ptr {}", func_ptr);

        auto arg_wrap = bb.assign(name + "_arg_wrap", "alloca {}", arg_type);
        std::print(bb.body().emplace_back(), "store {} {}, ptr {}", arg_type, arg, arg_wrap);

        auto args_ptr = bb.assign(name + "_args_ptr", "alloca [1 x ptr]");
        std::print(bb.body().emplace_back(), "store ptr {}, ptr {}", arg_wrap, args_ptr);
        auto args_inner
            = bb.assign(name + "_args_inner", "getelementptr inbounds [1 x ptr], ptr {}, i64 0, i64 0", args_ptr);
        auto launch_res
            = bb.assign(name,
                        "call i32 @{}(ptr {}, i32 {}, i32 1, i32 1, i32 {}, i32 1, i32 1, "
                        "i32 {}, ptr {}, ptr {}, ptr null)",
                        Cu_Launch_Kernel, func_inner, n_groups, n_items, shared_mem_bytes, stream, args_inner);
        emit_cu_error_handling(bb, launch_res);
        return ret_lam;
    }
    return std::nullopt;
}

void DeviceEmitter::start() {
    for (auto kernel : world().externals().muts()) {
        auto kernel_lam = kernel->expect_mut<Lam>("an external kernel to be a mutable lambda");
        kernels_.emplace(kernel_lam);
    }
    Super::start();
    return;
}

std::string DeviceEmitter::prepare() {
    auto is_kern = kernels_.contains(root());
    if (!is_kern) return Super::prepare();
    auto kernel = root();

    std::print(func_impls_, "define ptx_kernel {} {}(", convert_ret_pi(kernel->type()->ret_pi()), id(kernel));

    auto [m1, m3, m4, m5, group_id, item_id, smem, arg, ret_lam] = kernel->vars<9>();

    auto arg_name = id(arg);
    locals_[arg]  = arg_name;
    std::print(func_impls_, "{} {}) {{\n", convert(arg->type()), arg_name);

    auto& bb = lam2bb_[kernel];

    auto register_sreg_idx = [&](const Def* def, std::string_view sreg) {
        auto name        = id(def);
        auto type        = def->type();
        auto type_name   = convert(type);
        auto opt_idx_lit = Idx::isa_lit(type);
        if (!opt_idx_lit)
            fe::throwf(MIM_LL_NVPTX_BE "type of `{}` must be a statically-sized `Idx` but is `{}`", def, type);
        auto idx_lit = opt_idx_lit.value();
        locals_[def] = name;
        declare("i32 @llvm.nvvm.read.ptx.sreg.{}()", sreg);
        if (type_name == "i0") {
            locals_[def] = "0";
        } else if (type_name == "i32") {
            bb.assign(name, "call i32 @llvm.nvvm.read.ptx.sreg.{}()", sreg);
        } else if (idx_lit < (1u << 31)) {
            auto i32 = bb.assign(name + "i32", "call i32 @llvm.nvvm.read.ptx.sreg.{}()", sreg);
            bb.assign(name, "trunc i32 {} to {}", i32, type_name);
        } else {
            fe::throwf(MIM_LL_NVPTX_BE "warp ID too large; must fit into `I32`");
        }
    };
    register_sreg_idx(group_id, "ctaid.x");
    register_sreg_idx(item_id, "tid.x");

    auto shared_as = Lit::expect(world().annex<gpu::addr_space_shared>(), "the shared address space");
    if (auto sigma = smem->type()->isa<Sigma>()) {
        if (sigma->num_ops() != 0)
            fe::throwf(MIM_LL_NVPTX_BE "shared-memory variable must be an empty sigma, but got `{}`", smem->type());
    } else {
        auto ptr    = Axm::expect<mem::Ptr>(smem->type(), "a shared-memory pointer type");
        auto [T, a] = ptr->args<2>();
        if (Lit::expect(a, "an address space") != shared_as)
            fe::throwf(MIM_LL_NVPTX_BE "shared-memory variable must live in the shared address space, but got `{}`",
                       smem->type());
        auto name     = "@" + smem->unique_name();
        locals_[smem] = name;
        std::print(vars_decls_, "{} = internal addrspace({}) global {} undef\n", name, a, convert(T));
    }

    return kernel->unique_name();
}

std::optional<std::string> DeviceEmitter::isa_targetspecific_intrinsic(ll::BB& bb, const Def* def) {
    auto name = id(def);

    if (auto sync_work_items = Axm::isa<gpu::sync_work_items>(def)) {
        declare("void @llvm.nvvm.barrier0()");

        emit_unsafe(sync_work_items->arg(0));
        emit_unsafe(sync_work_items->arg(1));
        std::print(bb.body().emplace_back(), "call void @llvm.nvvm.barrier0()");
        return name;
    } else if (auto tri = Axm::isa<math::tri>(def)) {
        auto arg       = emit(tri->arg());
        auto type      = convert(tri->arg()->type());
        auto func_name = ""s;
        switch (tri.id()) {
            case math::tri::ahff: func_name = "sin"; break;
            case math::tri::ahfF: func_name = "cos"; break;
            case math::tri::ahFf: func_name = "tan"; break;
            case math::tri::ahFF: break;
            case math::tri::aHff: func_name = "sinh"; break;
            case math::tri::aHfF: func_name = "cosh"; break;
            case math::tri::aHFf: func_name = "tanh"; break;
            case math::tri::aHFF: break;
            case math::tri::Ahff: func_name = "asin"; break;
            case math::tri::AhfF: func_name = "acos"; break;
            case math::tri::AhFf: func_name = "atan"; break;
            case math::tri::AhFF: break;
            case math::tri::AHff: func_name = "asinh"; break;
            case math::tri::AHfF: func_name = "acosh"; break;
            case math::tri::AHFf: func_name = "atanh"; break;
            case math::tri::AHFF: break;
        }
        if (func_name.empty()) fe::throwf("Trigonometric tag used by {} is currently unused", def);
        func_name                = func_name + ll::detail::math_suffix(tri->arg()->type());
        auto libdevice_func_name = "__nv_" + func_name;
        declare("{} @{}({})", type, libdevice_func_name, type);
        uses_libdevice = true;
        bb.assign(name, "call {} @{}({} {})", type, libdevice_func_name, type, arg);
        return name;
    } else if (auto exp = Axm::isa<math::exp>(def)) {
        auto arg       = emit(exp->arg());
        auto type      = convert(exp->arg()->type());
        auto func_name = ""s;
        switch (exp.id()) {
            case math::exp::lbb: func_name = "exp"; break;
            case math::exp::lbB: func_name = "exp2"; break;
            case math::exp::lBb: func_name = "exp10"; break;
            case math::exp::lBB: break;
            case math::exp::Lbb: func_name = "log"; break;
            case math::exp::LbB: func_name = "log2"; break;
            case math::exp::LBb: func_name = "log10"; break;
            case math::exp::LBB: break;
        }
        if (func_name.empty()) fe::throwf("Exponential tag used by {} is currently unused", def);
        func_name                = func_name + ll::detail::math_suffix(exp->arg()->type());
        auto libdevice_func_name = "__nv_" + func_name;
        declare("{} @{}({})", type, libdevice_func_name, type);
        uses_libdevice = true;
        bb.assign(name, "call {} @{}({} {})", type, libdevice_func_name, type, arg);
        return name;
    }
    return std::nullopt;
}

void emit_host(World& world, std::ostream& ostream, std::optional<std::string> device_fatbin_file, ll::Emitter::Rt rt) {
    HostEmitter emitter(world, ostream, device_fatbin_file);
    emitter.rt_mode(rt);
    // Same one-liner the `ll` backend uses; each backend just names its own runtime module.
    if (rt == ll::Emitter::Rt::embed) emitter.load_rt_module("ll_nvptx_rt.ll");
    emitter.run();
}

DeviceEmitFlags emit_device(World& world, std::ostream& ostream) {
    DeviceEmitter emitter(world, ostream);
    emitter.run();

    return DeviceEmitFlags{
        .uses_libdevice = emitter.is_using_libdevice(),
    };
}

} // namespace mim::plug::ll_nvptx
