#include "mim/plug/ll_nvptx/phase/ll_nvptx.h"

#include <format>

#include <mim/driver.h>

#include <mim/util/sys.h>

#include <mim/plug/core/core.h>
#include <mim/plug/gpu/gpu.h>
#include <mim/plug/ll_nvptx/ll_nvptx.h>
#include <mim/plug/mem/mem.h>

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

    void emit_epilogue(Lam*) final;

    std::optional<std::string> isa_targetspecific_intrinsic(ll::BB&, const Def*) final;

protected:
    std::string convert(const Def*, bool simd = true) override;

private:
    static constexpr std::string_view mod_name_          = "@.mimir_cu_mod";
    static constexpr std::string_view ctx_name_          = "@.mimir_cu_ctx";
    static constexpr std::string_view fatbin_name_       = "@.fatbin";
    static constexpr std::string_view kernel_array_name_ = "@.mimir_kernels";
    static constexpr std::string_view kernel_name_prefix = "@.kname.";

    void emit_cu_error_handling(ll::BB&, const std::string&, bool at_tail = false);

    std::optional<std::string> device_fatbin_file_;
    LamMap<int> kernel_ids_;

    DefSet analyzed_;
};

class DeviceEmitter : public ll::Emitter {
public:
    using Super = ll::Emitter;

    DeviceEmitter(World& world, std::ostream& ostream)
        : Super(world, "llvm_nvptx_device_emitter", ostream) {}

    void start() final;
    void emit_epilogue(Lam*) override;

    std::string prepare() override;

    std::optional<std::string> isa_targetspecific_intrinsic(ll::BB&, const Def*) final;

    bool is_using_libdevice() const { return uses_libdevice; }
    const std::string& get_extra_flags() const { return extra_flags; }

private:
    std::string convert(const Def* def, bool simd = false) override {
        if (simd) WLOG("Ignoring simd=true for type conversion in device code.");
        return Super::convert(def, false);
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
        auto kernel_lam = kernel->isa_mut<Lam>();
        assert(kernel_lam && "Expect kernel passed to %gpu.launch to be a mutable lambda");
        if (kernel_ids_.contains(kernel_lam)) return;
        auto kid                = kernel_ids_.size();
        kernel_ids_[kernel_lam] = kid;
    }
}

constexpr auto CU_INIT                = "cuInit";
constexpr auto CU_CTX_CREATE          = "cuCtxCreate_v4";
constexpr auto CU_CTX_DESTROY         = "cuCtxDestroy_v2";
constexpr auto CU_DEVICE_GET          = "cuDeviceGet";
constexpr auto CU_LAUNCH_KERNEL       = "cuLaunchKernel_ptsz";
constexpr auto CU_MEM_ALLOC           = "cuMemAlloc_v2";
constexpr auto CU_MEM_ALLOC_ASYNC     = "cuMemAllocAsync_ptsz";
constexpr auto CU_MEM_FREE            = "cuMemFree_v2";
constexpr auto CU_MEM_FREE_ASYNC      = "cuMemFreeAsync_ptsz";
constexpr auto CU_MEMCPY_HTOD         = "cuMemcpyHtoD_v2";
constexpr auto CU_MEMCPY_HTOD_ASYNC   = "cuMemcpyHtoDAsync_v2_ptsz";
constexpr auto CU_MEMCPY_DTOH         = "cuMemcpyDtoH_v2";
constexpr auto CU_MEMCPY_DTOH_ASYNC   = "cuMemcpyDtoHAsync_v2_ptsz";
constexpr auto CU_MODULE_LOAD_FATBIN  = "cuModuleLoadFatBinary";
constexpr auto CU_MODULE_GET_FUNCTION = "cuModuleGetFunction";
constexpr auto CU_MODULE_UNLOAD       = "cuModuleUnload";
constexpr auto CU_STREAM_CREATE       = "cuStreamCreate";
constexpr auto CU_STREAM_DESTROY      = "cuStreamDestroy_v2";
constexpr auto CU_STREAM_SYNC         = "cuStreamSynchronize_ptsz";

void HostEmitter::emit_cu_error_handling(ll::BB& bb, const std::string& cu_result, bool tail) {
    // TODO: implement
    return;
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

void HostEmitter::emit_epilogue(Lam* lam) {
    auto& bb = lam2bb_[lam];

    // HACK: we partially re-implement the checks in Super::emit_epilogue to catch target-specific applications
    auto app = lam->body()->as<App>();
    if (auto ret = isa_targetspecific_intrinsic(bb, app)) {
        assert(ret.has_value());
        if (app->callee() == root()->ret_var()) // return
            assert(false && "Return not implemented in NVPTX backend");
        else if (auto dispatch = Dispatch(app))
            assert(false && "Dispatch not implemented in NVPTX backend");
        else if (app->callee()->isa<Bot>())
            assert(false && "Bot not implemented in NVPTX backend");
        else if (auto _ = Lam::isa_mut_basicblock(app->callee())) // ordinary jump
            assert(false && "Ordinary Jump not implemented in NVPTX backend");
        else if (Pi::isa_returning(app->callee_type())) // function call
            bb.tail("br label {}", ret.value());
        else
            assert(false && "Unexpected return case in NVPTX backend");
    } else {
        Super::emit_epilogue(lam);
    }
}

std::optional<std::string> HostEmitter::isa_targetspecific_intrinsic(ll::BB& bb, const Def* def) {
    auto name = id(def);
    std::string op;

    if (auto default_stream = Axm::isa<gpu::default_stream>(def)) {
        return "null";
    } else if (auto init = Axm::isa<gpu::init>(def)) {
        auto dev_num   = 0; // TODO: consider parameterizing this
        auto ctx_flags = 0; // TODO: consider parameterizing this

        declare("i32 @{}(i32)", CU_INIT);
        auto init_res = bb.assign(name + "_init_res", "call i32 @{}(i32 0)", CU_INIT);
        emit_cu_error_handling(bb, init_res);

        declare("i32 @{}(ptr, i32)", CU_DEVICE_GET);
        auto dev_ptr = bb.assign(name + "_dev_ptr", "alloca i32");
        auto dev_get_res
            = bb.assign(name + "_get_res", "call i32 @{}(ptr {}, i32 {})", CU_DEVICE_GET, dev_ptr, dev_num);
        emit_cu_error_handling(bb, dev_get_res);

        declare("i32 @{}(ptr, ptr, i32, i32)", CU_CTX_CREATE);
        std::print(vars_decls_, "{} = global ptr null\n", ctx_name_);
        auto dev     = bb.assign(name + "_dev", "load i32, ptr {}", dev_ptr);
        auto ctx_res = bb.assign(name + "_ctx_res", "call i32 @{}(ptr {}, ptr null, i32 {}, i32 {})", CU_CTX_CREATE,
                                 ctx_name_, ctx_flags, dev);
        emit_cu_error_handling(bb, ctx_res);

        declare("i32 @{}(ptr, ptr)", CU_MODULE_LOAD_FATBIN);
        std::print(vars_decls_, "{} = global ptr null\n", mod_name_);
        if (device_fatbin_file_.has_value()) {
            std::ifstream fatbin_file(device_fatbin_file_.value(), std::ios::binary);
            if (!fatbin_file) error("Could not open {} as binary file", device_fatbin_file_.value());

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
        auto mod_res = bb.assign(name + "_mod_res", "call i32 @{}(ptr {}, ptr {})", CU_MODULE_LOAD_FATBIN, mod_name_,
                                 fatbin_name_);
        emit_cu_error_handling(bb, mod_res);
        auto mod_inner = bb.assign(name + "_mod_inner", "load ptr, ptr {}", mod_name_);

        declare("i32 @{}(ptr, ptr, ptr)", CU_MODULE_GET_FUNCTION);
        for (auto [kernel, kid] : kernel_ids_) {
            auto kname    = id(kernel).substr(1);
            auto func_ptr = bb.assign("%" + kname + "_funcptr", "getelementptr inbounds ptr, ptr {}, i64 {}",
                                      kernel_array_name_, kid);
            auto func_res = bb.assign("%" + kname + "_getfuncres", "call i32 @{}(ptr {}, ptr {}, ptr {}{})",
                                      CU_MODULE_GET_FUNCTION, func_ptr, mod_inner, kernel_name_prefix, kid);
            emit_cu_error_handling(bb, func_res);
        }

        auto mem = init->arg();
        return emit_unsafe(mem);
    } else if (auto deinit = Axm::isa<gpu::deinit>(def)) {
        declare("i32 @{}(ptr)", CU_MODULE_UNLOAD);
        bb.tail("{}_mod = load ptr, ptr {}", name, mod_name_);
        bb.tail("{}_mod_unload_res = call i32 @{}(ptr {}_mod)", name, CU_MODULE_UNLOAD, name);
        emit_cu_error_handling(bb, name + "_mod_unload_res", true);

        declare("i32 @{}(ptr)", CU_CTX_DESTROY);
        bb.tail("{}_ctx = load ptr, ptr {}", name, ctx_name_);
        bb.tail("{}_ctx_destroy_res = call i32 @{}(ptr {}_ctx)", name, CU_CTX_DESTROY, name);
        emit_cu_error_handling(bb, name + "_ctx_destroy_res", true);

        emit_unsafe(deinit->arg(0));
        return emit_unsafe(deinit->arg(1));
    } else if (auto stream_init = Axm::isa<gpu::stream_init>(def)) {
        declare("i32 @{}(ptr, i32)", CU_STREAM_CREATE);

        emit_unsafe(stream_init->arg(0));
        emit_unsafe(stream_init->arg(1));
        auto stream_ptr = emit(stream_init->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {}, i32 0)", CU_STREAM_CREATE, stream_ptr);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto stream_deinit = Axm::isa<gpu::stream_deinit>(def)) {
        declare("i32 @{}(ptr)", CU_STREAM_DESTROY);

        emit_unsafe(stream_deinit->arg(0));
        emit_unsafe(stream_deinit->arg(1));
        auto stream = emit(stream_deinit->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {})", CU_STREAM_DESTROY, stream);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto stream_sync = Axm::isa<gpu::stream_sync>(def)) {
        declare("i32 @{}(ptr)", CU_STREAM_SYNC);

        emit_unsafe(stream_sync->arg(0));
        emit_unsafe(stream_sync->arg(1));
        auto stream = emit(stream_sync->arg(2));

        auto res = bb.assign(name, "call i32 @{}(ptr {})", CU_STREAM_SYNC, stream);
        emit_cu_error_handling(bb, res);
        return res;
    } else if (auto alloc = Axm::isa<gpu::alloc>(def)) {
        bool is_async;
        switch (alloc.id()) {
            case gpu::alloc::block: is_async = false; break;
            case gpu::alloc::asyn: is_async = true; break;
            default: fe::unreachable();
        }

        if (is_async)
            declare("i32 @{}(ptr, i64, ptr)", CU_MEM_ALLOC_ASYNC);
        else
            declare("i32 @{}(ptr, i64)", CU_MEM_ALLOC);

        emit_unsafe(alloc->arg(0));
        auto alloc_t    = alloc->decurry()->arg();
        World& w        = alloc_t->world();
        auto type_size  = w.call(core::trait::size, alloc_t);
        auto alloc_size = emit(type_size);

        auto ptr_t = convert(Axm::as<mem::Ptr>(def->proj(1)->type()));

        auto alloc_ptr = bb.assign(name + "ptr", "alloca {}", ptr_t);
        std::string alloc_res;
        if (is_async) {
            auto stream = emit(alloc->arg(1));
            alloc_res   = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, ptr {})", CU_MEM_ALLOC_ASYNC, alloc_ptr,
                                    alloc_size, stream);
        } else
            alloc_res = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {})", CU_MEM_ALLOC, alloc_ptr, alloc_size);

        emit_cu_error_handling(bb, alloc_res);
        return bb.assign(name, "load {}, {} addrspace(0)* {}", ptr_t, ptr_t, alloc_ptr);
    } else if (auto free = Axm::isa<gpu::free>(def)) {
        bool is_async;
        switch (free.id()) {
            case gpu::free::block: is_async = false; break;
            case gpu::free::asyn: is_async = true; break;
            default: fe::unreachable();
        }

        if (is_async)
            declare("i32 @{}(i64)", CU_MEM_FREE_ASYNC);
        else
            declare("i32 @{}(i64)", CU_MEM_FREE);

        emit_unsafe(free->arg(0));
        auto ptr = emit(free->arg(1));

        std::string free_res;
        if (is_async) {
            auto stream = emit(free->arg(2));
            free_res    = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {})", CU_MEM_FREE_ASYNC, ptr, stream);
        } else
            free_res = bb.assign(name + "res", "call i32 @{}(i64 {})", CU_MEM_FREE, ptr);

        emit_cu_error_handling(bb, free_res);
        return free_res;
    } else if (auto copy_to_device = Axm::isa<gpu::copy_to_device>(def)) {
        bool is_async;
        switch (copy_to_device.id()) {
            case gpu::copy_to_device::block: is_async = false; break;
            case gpu::copy_to_device::asyn: is_async = true; break;
            default: fe::unreachable();
        }

        if (is_async)
            declare("i32 @{}(i64, ptr, i64, ptr)", CU_MEMCPY_HTOD_ASYNC);
        else
            declare("i32 @{}(i64, ptr, i64)", CU_MEMCPY_HTOD);

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
            copy_res    = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {}, i64 {}, ptr {})", CU_MEMCPY_HTOD_ASYNC,
                                    dev_ptr, host_ptr, size, stream);
        } else
            copy_res = bb.assign(name + "res", "call i32 @{}(i64 {}, ptr {}, i64 {})", CU_MEMCPY_HTOD, dev_ptr,
                                 host_ptr, size);

        emit_cu_error_handling(bb, copy_res);
        return copy_res;
    } else if (auto copy_to_host = Axm::isa<gpu::copy_to_host>(def)) {
        bool is_async;
        switch (copy_to_host.id()) {
            case gpu::copy_to_host::block: is_async = false; break;
            case gpu::copy_to_host::asyn: is_async = true; break;
            default: fe::unreachable();
        }
        if (is_async)
            declare("i32 @{}(ptr, i64, i64, ptr)", CU_MEMCPY_DTOH_ASYNC);
        else
            declare("i32 @{}(ptr, i64, i64)", CU_MEMCPY_DTOH);

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
            copy_res    = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, i64 {}, ptr {})", CU_MEMCPY_DTOH_ASYNC,
                                    host_ptr, dev_ptr, size, stream);
        } else
            copy_res = bb.assign(name + "res", "call i32 @{}(ptr {}, i64 {}, i64 {})", CU_MEMCPY_DTOH, host_ptr,
                                 dev_ptr, size);

        emit_cu_error_handling(bb, copy_res);
        return copy_res;
    } else if (auto launch = Axm::isa<gpu::launch>(def)) {
        // TODO: rewrite to use modern cuLaunchKernelEx instead
        declare("i32 @{}(ptr, i32, i32, i32, i32, i32, i32, i32, ptr, ptr, ptr)", CU_LAUNCH_KERNEL);

        auto [implicits, launch_config, kernel_def, arg_def, func_args] = launch->uncurry_args<5>();
        auto [n_groups_def, n_items_def, stream_def, m, MT]             = launch_config->projs<5>();
        auto [mem, ret_lam_def]                                         = func_args->projs<2>();

        Lam* lam = kernel_def->isa_mut<Lam>();
        if (!lam) error("kernel is not a lamda {}", kernel_def);
        if (!kernel_ids_.contains(lam)) error("unknown kernel {}", lam);
        auto kid = kernel_ids_[lam];

        auto shared_mem_bytes = 0;
        if (auto smem_count = Lit::as(m)) {
            if (smem_count != 1) error("You can only have one dynamic allocation of shared memory per kernel");
            shared_mem_bytes = Lit::as(world().call(core::trait::size, MT));
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
                        CU_LAUNCH_KERNEL, func_inner, n_groups, n_items, shared_mem_bytes, stream, args_inner);
        emit_cu_error_handling(bb, launch_res);
        return ret_lam;
    }
    return std::nullopt;
}

void DeviceEmitter::emit_epilogue(Lam* lam) {
    auto& bb = lam2bb_[lam];

    // HACK: we partially re-implement the checks in Super::emit_epilogue to catch target-specific applications
    auto app = lam->body()->as<App>();
    if (auto mslot = Axm::isa<mem::mslot>(app)) {
        // Continuation-based stack slot: allocate and jump to the passed continuation with the fresh pointer.
        auto [Ta, rest]   = mslot->uncurry_args<2>();
        auto [T, a]       = Ta->projs<2>();
        auto [msize, ret] = rest->projs<2>();
        emit_unsafe(msize->proj(0)); // mem
        // TODO array with size
        auto ret_lam = ret->as_mut<Lam>();
        auto ptr     = ret_lam->var(2, 1);
        auto v_ptr   = "@" + app->unique_name() + ".slot";
        std::print(vars_decls_, "{} = internal addrspace({}) global {} undef\n", v_ptr, a, convert(T));
        lam2bb_[ret_lam].phis[ptr].emplace_back(v_ptr, id(lam, true));
        globals_[ptr] = id(ptr);
        return bb.tail("br label {}", id(ret_lam));
    } else {
        Super::emit_epilogue(lam);
    }
}

void DeviceEmitter::start() {
    for (auto kernel : world().externals().muts()) {
        auto kernel_lam = kernel->isa_mut<Lam>();
        assert(kernel_lam && "Expect kernel passed to %gpu.launch to be a mutable lambda");
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
        if (!opt_idx_lit) error("Type of '{}' must have known index type but has {}", def, type);
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
            error("Warp ID too large, must fit into I32");
        }
    };
    register_sreg_idx(group_id, "ctaid.x");
    register_sreg_idx(item_id, "tid.x");

    auto shared_as = Lit::as(world().annex<gpu::addr_space_shared>());
    if (auto sigma = smem->type()->isa<Sigma>()) {
        assert(sigma->num_ops() == 0 && "Expect empty sigma for shared memory variable");
    } else {
        auto ptr = Axm::isa<mem::Ptr>(smem->type());
        assert(ptr && "Expect pointer type for shared memory variable");
        auto [T, a] = ptr->args<2>();
        assert(Lit::as(a) == shared_as && "Expect shared memory pointer type for shared memory variable");
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
    }
    return std::nullopt;
}

void emit_host(World& world, std::ostream& ostream, std::optional<std::string> device_fatbin_file) {
    HostEmitter emitter(world, ostream, device_fatbin_file);
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
