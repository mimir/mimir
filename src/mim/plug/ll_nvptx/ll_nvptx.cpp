#include "mim/plug/ll_nvptx/phase/ll_nvptx.h"

#include <mim/driver.h>
#include <mim/plugin.h>

#include <mim/util/sys.h>

#include <mim/plug/gpu/gpu.h>

#include "mim/plug/ll_nvptx/ll_nvptx.h"

using namespace std::string_literals;

namespace mim::plug::ll_nvptx {

namespace {

class CmdNotFound : public std::logic_error {
public:
    CmdNotFound(const std::string& s)
        : std::logic_error(s) {}
};

constexpr auto default_compute_cap = "75";

std::string get_compute_capability() {
    auto nvidia_smi = sys::find_cmd("nvidia-smi");
    if (!std::filesystem::exists(nvidia_smi)) error<CmdNotFound>("Could not find command: nvidia-smi {}", nvidia_smi);
    auto out = sys::exec(std::format("{} --query-gpu=compute_cap --format=csv,noheader", nvidia_smi));
    out.erase(std::remove_if(out.begin(), out.end(), ::isspace), out.end());
    // out should now have form "7.5" referencing the compute capability "sm_75"

    auto dot_pos = out.find('.');
    assert(dot_pos < out.size());

    for (size_t i = 0; i < out.size(); ++i) {
        if (i == dot_pos) continue;
        if (!std::isdigit(out[i])) {
            std::println(std::cerr, "Could not determine compute capability, continuing with default: '{}'.",
                         default_compute_cap);
            return default_compute_cap;
        }
    }

    auto compute_cap = std::format("{}{}", out.substr(0, dot_pos), out.substr(dot_pos + 1));
    std::println(std::cout, "Determined compute capability to be '{}'", compute_cap);
    return compute_cap;
}

constexpr auto LIBDEVICE_NAME = "libdevice.10.bc"s;

std::optional<std::filesystem::path> parse_nvcc_profile(const std::filesystem::path& cuda_bin_path) {
    auto profile_path = cuda_bin_path / "nvcc.profile";
    if (!std::filesystem::exists(profile_path)) return std::nullopt;

    std::ifstream file(profile_path);
    if (!file.is_open()) return std::nullopt;

    std::string line;
    std::string top_dir = "";
    std::string lib_dir = "";

    while (std::getline(file, line)) {
        line.erase(std::remove_if(line.begin(), line.end(), ::isspace), line.end());
        if (line.starts_with("TOP=")) {
            size_t macro_pos = line.find("$(_HERE_)/");
            top_dir          = line.substr(macro_pos + 10);
        }
        if (line.rfind("NVVMIR_LIBRARY_DIR=", 0) == 0) {
            size_t macro_pos = line.find("$(TOP)/");
            lib_dir          = line.substr(macro_pos + 7);
        }
    }
    if (top_dir.empty() || lib_dir.empty()) return std::nullopt;
    auto path          = cuda_bin_path / top_dir / lib_dir / LIBDEVICE_NAME;
    auto resolved_path = path.lexically_normal();
    if (!std::filesystem::exists(resolved_path)) return std::nullopt;
    return resolved_path;
}

std::string find_libdevice() {
    auto nvcc_which = sys::find_cmd("nvcc");
    if (nvcc_which.find("not found") == std::string::npos) {
        auto nvcc_path     = std::filesystem::canonical(nvcc_which);
        auto cuda_bin_path = nvcc_path.parent_path();
        if (auto libdevice_path = parse_nvcc_profile(cuda_bin_path)) return libdevice_path->string();
    }
    if (const char* cuda_home_env = std::getenv("CUDA_HOME")) {
        auto libdevice_path = std::filesystem::path(cuda_home_env) / "nvvm" / "libdevice" / LIBDEVICE_NAME;
        if (std::filesystem::exists(libdevice_path)) return libdevice_path.string();
    }
    auto debian_fallback = "/usr/lib/nvidia-cuda-toolkit/libdevice/"s + LIBDEVICE_NAME;
    if (std::filesystem::exists(debian_fallback)) return debian_fallback;

    error<CmdNotFound>("Unable to find libdevice. Try setting the CUDA_HOME environment variable.");
}

void link_libdevice(const std::string& libdevice_path, const std::string& in_name, const std::string& out_name) {
    if (!std::filesystem::exists(libdevice_path)) error("libdevice path does not exist: {}", libdevice_path);
    auto llvm_link = sys::find_cmd("llvm-link");
    if (!std::filesystem::exists(llvm_link)) error<CmdNotFound>("Could not find command: llvm-link {}", llvm_link);
    auto cmd = std::format("{} {} {} -o {}", llvm_link, in_name, libdevice_path, out_name);
    auto rc  = sys::system(cmd);
    if (rc != 0) error("Command exited with error code {}", rc);
}

void optimize_bytecode(const std::string& in_name, const std::string& out_name) {
    auto opt = sys::find_cmd("opt");
    if (!std::filesystem::exists(opt)) error<CmdNotFound>("Could not find command: opt {}", opt);
    // TODO: consider adding more (NVPTX-specific) passes
    // TODO: consider setting other optimization level
    auto passes = "default<O2>,nvvm-reflect";
    auto cmd    = std::format("{} -passes=\"{}\" {} -o {}", opt, passes, in_name, out_name);
    auto rc     = sys::system(cmd);
    if (rc != 0) error("Command exited with error code {}", rc);
}

void compile2ptx(const std::string& compute_cap, const std::string& in_name, const std::string& out_name) {
    auto llc = sys::find_cmd("llc");
    if (!std::filesystem::exists(llc)) error<CmdNotFound>("Could not find command: llc {}", llc);
    // TODO: support 32-bit version?
    // TODO: consider setting other optimization level - currently llc's default: -O2
    auto cmd = std::format("{} -march=nvptx64 -mcpu=sm_{} {} -o {}", llc, compute_cap, in_name, out_name);
    auto rc  = sys::system(cmd);
    if (rc != 0) error("Command exited with error code {}", rc);
}

void compile2cubin(const std::string& compute_cap, const std::string& in_name, const std::string& out_name) {
    auto ptxas = sys::find_cmd("ptxas");
    if (!std::filesystem::exists(ptxas)) error<CmdNotFound>("Could not find command: ptxas {}", ptxas);
    // TODO: consider setting other optimization level - currently ptxas' default: -O3
    auto cmd = std::format("{} -arch=sm_{} {} -o {}", ptxas, compute_cap, in_name, out_name);
    auto rc  = sys::system(cmd);
    if (rc != 0) error("Command exited with error code {}", rc);
}

void compile2fatbin(const std::string& compute_cap, const std::string& in_name, const std::string& out_name) {
    auto fatbinary = sys::find_cmd("fatbinary");
    if (!std::filesystem::exists(fatbinary)) error<CmdNotFound>("Could not find command: fatbinary {}", fatbinary);
    auto cmd
        = std::format("{} --create={} -64 --image3=kind=elf,sm={},file={}", fatbinary, out_name, compute_cap, in_name);
    auto rc = sys::system(cmd);
    if (rc != 0) error("Command exited with error code {}", rc);
}

} // namespace

class Emit : public Phase {
public:
    Emit(World& world, flags_t annex)
        : Phase(world, annex) {}

    void start() override {
        auto name = world().name() ? std::string(world().name().view()) : "a"s;

        const auto host_ll_name    = name + ".ll"s;
        const auto dev_ll_name     = name + "_dev.ll"s;
        const auto dev_ptx_name    = name + "_dev.ptx"s;
        const auto dev_cubin_name  = name + "_dev.cubin"s;
        const auto dev_fatbin_name = name + "_dev.fatbin"s;
        const auto dev_bc_raw_name = name + "_dev_raw.bc"s;
        const auto dev_bc_opt_name = name + "_dev_opt.bc"s;

        auto host_ofs = std::ofstream(host_ll_name);

        auto split_apply_phase = Phase::create(world().driver().phases(), world().annex<gpu::split_apply>());
        auto setup_phase       = split_apply_phase.get()->as<RWPhase>();
        setup_phase->run();

        DeviceEmitFlags device_flags;
        {
            auto dev_ofs = std::ofstream(dev_ll_name);
            device_flags = emit_device(setup_phase->new_world(), dev_ofs);
        }

        bool embed_device_code;
#ifdef __linux__
        try {
            embed_device_code = true;
            auto compute_cap  = get_compute_capability();
            if (device_flags.uses_libdevice) {
                auto libdevice_path = find_libdevice();
                link_libdevice(libdevice_path, dev_ll_name, dev_bc_raw_name);
                optimize_bytecode(dev_bc_raw_name, dev_bc_opt_name);
            }
            auto compile_input = device_flags.uses_libdevice ? dev_bc_opt_name : dev_ll_name;
            compile2ptx(compute_cap, compile_input, dev_ptx_name);
            compile2cubin(compute_cap, dev_ptx_name, dev_cubin_name);
            compile2fatbin(compute_cap, dev_cubin_name, dev_fatbin_name);
        } catch (const CmdNotFound& e) {
            WLOG("{}\nFalling back to not embedding device code.", e.what());
            embed_device_code = false;
        }
#else
        embed_device_code = false;
#endif
        auto device_fatbin_file = embed_device_code ? std::optional(dev_fatbin_name) : std::nullopt;
        emit_host(setup_phase->old_world(), host_ofs, device_fatbin_file);

        if (embed_device_code) {
            std::println(std::cout, "Unified (Fat) LLVM IR written to {}", host_ll_name);
        } else {
            std::println(std::cout, "Host-only LLVM IR written to {}", host_ll_name);
            std::println(std::cout, "Device-only LLVM IR written to {}", dev_ll_name);
        }
    }
};

} // namespace mim::plug::ll_nvptx

using namespace mim;

static void reg_phases(Flags2Phases& phases) { Phase::hook<plug::ll_nvptx::emit, plug::ll_nvptx::Emit>(phases); }

extern "C" MIM_EXPORT Plugin mim_get_plugin() { return {"ll_nvptx", MIM_VERSION, nullptr, reg_phases}; }
