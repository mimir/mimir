#include "mim/plug/ll_nvptx/phase/ll_nvptx.h"

#include <mim/driver.h>
#include <mim/plugin.h>

#include <mim/util/sys.h>

#include <mim/plug/gpu/gpu.h>

#include "mim/plug/ll_nvptx/ll_nvptx.h"

using namespace std::string_literals;
using namespace std::string_view_literals;

namespace mim::plug::ll_nvptx {

namespace {

struct NvptxCompileArgs {
    std::string host_ll_name, dev_ll_name, dev_ptx_name, dev_cubin_name, dev_fatbin_name, dev_bc_raw_name,
        dev_bc_opt_name;
#ifdef __linux__
    bool embed_device_code = true;
#else
    bool embed_device_code = false;
#endif
    bool embed_ptx   = true;
    bool embed_cubin = true;
    std::string compute_cap, libdevice_path;
    std::string link_llvm_args, opt_args = R"(-passes="default<O2>,nvvm-reflect")", llc_args, ptxas_args,
                                fatbinary_args;
};

constexpr auto Default_Compute_Cap = "75";

std::string get_compute_capability() {
    auto nvidia_smi = sys::require_cmd("nvidia-smi");
    auto out        = sys::exec(std::format("{} --query-gpu=compute_cap --format=csv,noheader", nvidia_smi));
    std::erase_if(out, ::isspace);
    // out should now have form "7.5" referencing the compute capability "sm_75"

    auto dot_pos = out.find('.');
    if (dot_pos == std::string::npos) {
        std::println(std::cerr, "Could not determine compute capability, continuing with default: '{}'.",
                     Default_Compute_Cap);
        return Default_Compute_Cap;
    }

    for (size_t i = 0; i < out.size(); ++i) {
        if (i == dot_pos) continue;
        if (!std::isdigit(out[i])) {
            std::println(std::cerr, "Could not determine compute capability, continuing with default: '{}'.",
                         Default_Compute_Cap);
            return Default_Compute_Cap;
        }
    }

    auto compute_cap = std::format("{}{}", out.substr(0, dot_pos), out.substr(dot_pos + 1));
    std::println(std::cout, "Determined compute capability to be '{}'", compute_cap);
    return compute_cap;
}

constexpr auto Libdevice_Name = "libdevice.10.bc"sv;

std::optional<std::filesystem::path> parse_nvcc_profile(const std::filesystem::path& cuda_bin_path) {
    auto profile_path = cuda_bin_path / "nvcc.profile";
    if (!std::filesystem::exists(profile_path)) return std::nullopt;

    std::ifstream file(profile_path);
    if (!file.is_open()) return std::nullopt;

    std::string line, top_dir, lib_dir;

    while (std::getline(file, line)) {
        std::erase_if(line, ::isspace);
        if (line.starts_with("TOP=")) {
            auto macro_pos = line.find("$(_HERE_)/");
            if (macro_pos == std::string::npos) break;
            top_dir = line.substr(macro_pos + 10);
        }
        if (line.starts_with("NVVMIR_LIBRARY_DIR=")) {
            auto macro_pos = line.find("$(TOP)/");
            if (macro_pos == std::string::npos) break;
            lib_dir = line.substr(macro_pos + 7);
        }
    }
    if (top_dir.empty() || lib_dir.empty()) return std::nullopt;
    auto path          = cuda_bin_path / top_dir / lib_dir / Libdevice_Name;
    auto resolved_path = path.lexically_normal();
    if (!std::filesystem::exists(resolved_path)) return std::nullopt;
    return resolved_path;
}

std::string find_libdevice() {
    auto nvcc = sys::find_cmd("nvcc");
    if (std::filesystem::exists(nvcc)) {
        auto nvcc_path     = std::filesystem::canonical(nvcc);
        auto cuda_bin_path = nvcc_path.parent_path();
        if (auto libdevice_path = parse_nvcc_profile(cuda_bin_path)) return libdevice_path->string();
    }
    if (const char* cuda_home_env = std::getenv("CUDA_HOME")) {
        auto libdevice_path = std::filesystem::path(cuda_home_env) / "nvvm" / "libdevice" / Libdevice_Name;
        if (std::filesystem::exists(libdevice_path)) return libdevice_path.string();
    }
    auto debian_fallback = std::filesystem::path("/usr/lib/nvidia-cuda-toolkit/libdevice/") / Libdevice_Name;
    if (std::filesystem::exists(debian_fallback)) return debian_fallback.string();

    fe::throwf<sys::CmdNotFound>("Unable to find '{}'. Try setting the CUDA_HOME environment variable.",
                                 Libdevice_Name);
}

void link_libdevice(const NvptxCompileArgs& c) {
    if (!std::filesystem::exists(c.libdevice_path)) fe::throwf("libdevice path does not exist: {}", c.libdevice_path);
    auto llvm_link = sys::require_cmd("llvm-link");
    sys::require_run(std::format("{} {} {} {} -o {}", llvm_link, c.link_llvm_args, c.dev_ll_name, c.libdevice_path,
                                 c.dev_bc_raw_name));
}

void optimize_bytecode(const NvptxCompileArgs& c) {
    auto opt = sys::require_cmd("opt");
    sys::require_run(std::format("{} {} {} -o {}", opt, c.opt_args, c.dev_bc_raw_name, c.dev_bc_opt_name));
}

void compile2ptx(const NvptxCompileArgs& c, bool uses_libdevice) {
    auto compile_input = uses_libdevice ? c.dev_bc_opt_name : c.dev_ll_name;
    auto llc           = sys::require_cmd("llc");
    sys::require_run(std::format("{} -march=nvptx64 -mcpu=sm_{} {} {} -o {}", llc, c.compute_cap, c.llc_args,
                                 compile_input, c.dev_ptx_name));
}

void compile2cubin(const NvptxCompileArgs& c) {
    auto ptxas = sys::require_cmd("ptxas");
    sys::require_run(std::format("{} -arch=sm_{} {} {} -o {}", ptxas, c.compute_cap, c.ptxas_args, c.dev_ptx_name,
                                 c.dev_cubin_name));
}

void compile2fatbin(const NvptxCompileArgs& c) {
    auto fatbinary = sys::require_cmd("fatbinary");
    auto ptx_args  = ""s;
    if (c.embed_ptx) {
        ptx_args = std::format("--image3=kind=ptx,sm={},file={}", c.compute_cap, c.dev_ptx_name);
        if (!c.ptxas_args.empty()) ptx_args += std::format(" --cmdline={}", c.ptxas_args);
    }
    auto cubin_args = ""s;
    if (c.embed_cubin) cubin_args = std::format("--image3=kind=elf,sm={},file={}", c.compute_cap, c.dev_cubin_name);
    sys::require_run(std::format("{} --create={} -64 {} {} {}", fatbinary, c.dev_fatbin_name, c.fatbinary_args,
                                 ptx_args, cubin_args));
}

} // namespace

class Emit : public Phase {
public:
    Emit(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? std::string(world().name().view()) : "a"s;

        auto c            = NvptxCompileArgs{};
        c.host_ll_name    = name + ".ll"s;
        c.dev_ll_name     = name + "_dev.ll"s;
        c.dev_ptx_name    = name + "_dev.ptx"s;
        c.dev_cubin_name  = name + "_dev.cubin"s;
        c.dev_fatbin_name = name + "_dev.fatbin"s;
        c.dev_bc_raw_name = name + "_dev_raw.bc"s;
        c.dev_bc_opt_name = name + "_dev_opt.bc"s;

        auto rt = ll::Emitter::Rt::embed;
        for (const auto& arg : args()) {
            world().DLOG("ll backend arg: `{}`", arg);
            // clang-format off
            if (false) {}
            else if (arg.starts_with("o="))          c.host_ll_name      = arg.substr(2);
            else if (arg.starts_with("output="))     c.host_ll_name      = arg.substr(7);
            else if (arg.starts_with("o-dev="))      c.dev_ll_name       = arg.substr(6);
            else if (arg.starts_with("output-dev=")) c.dev_ll_name       = arg.substr(11);
            else if (arg.starts_with("sm="))         c.compute_cap       = arg.substr(3);
            else if (arg.starts_with("libdevice="))  c.libdevice_path    = arg.substr(10);
            else if (arg.starts_with("Xlink_llvm=")) c.link_llvm_args    = arg.substr(11);
            else if (arg.starts_with("Xopt="))       c.opt_args          = arg.substr(5);
            else if (arg.starts_with("Xllc="))       c.llc_args          = arg.substr(5);
            else if (arg.starts_with("Xptxas="))     c.ptxas_args        = arg.substr(7);
            else if (arg.starts_with("Xfatbinary=")) c.fatbinary_args    = arg.substr(11);
            else if (arg == "no-embed")              c.embed_device_code = false;
            else if (arg == "no-ptx-embed")          c.embed_ptx         = false;
            else if (arg == "no-cubin-embed")        c.embed_cubin       = false;
            else if (arg == "rt=embed")              rt                  = ll::Emitter::Rt::embed;
            else if (arg == "rt=extern")             rt                  = ll::Emitter::Rt::ext;
            // clang-format on
        }

        auto split_apply_phase = Phase::create(world().driver().phases(), world().annex<gpu::split_apply>());
        auto setup_phase       = split_apply_phase.get()->expect<RWPhase>("%gpu.split_apply to be an RWPhase");
        setup_phase->run();

        DeviceEmitFlags device_flags;
        {
            auto dev_ofs = std::ofstream(c.dev_ll_name);
            device_flags = emit_device(setup_phase->new_world(), dev_ofs);
        }
        if (c.embed_device_code) {
            if (!c.embed_ptx && !c.embed_cubin)
                fe::throwf("Embedding requested with no images (neither PTX nor CUBIN).");
            try {
                if (c.compute_cap.empty()) c.compute_cap = get_compute_capability();
                if (device_flags.uses_libdevice) {
                    if (c.libdevice_path.empty()) c.libdevice_path = find_libdevice();
                    link_libdevice(c);
                    optimize_bytecode(c);
                }
                compile2ptx(c, device_flags.uses_libdevice);
                compile2cubin(c);
                compile2fatbin(c);
            } catch (const sys::CmdNotFound& e) {
                WLOG("{}", e.what());
                WLOG("Falling back to not embedding device code.");
                c.embed_device_code = false;
            }
        }
        auto device_fatbin_file = c.embed_device_code ? std::optional(c.dev_fatbin_name) : std::nullopt;
        auto host_ofs           = std::ofstream(c.host_ll_name);
        emit_host(setup_phase->old_world(), host_ofs, device_fatbin_file, rt);

        if (c.embed_device_code) {
            std::println(std::cout, "Unified (Fat) LLVM IR written to {}", c.host_ll_name);
        } else {
            std::println(std::cout, "Host-only LLVM IR written to {}", c.host_ll_name);
            std::println(std::cout, "Device-only LLVM IR written to {}", c.dev_ll_name);
        }
    }
};

} // namespace mim::plug::ll_nvptx

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::ll_nvptx::emit, plug::ll_nvptx::Emit>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"ll_nvptx", MIM_VERSION, nullptr, reg_phases}; }
