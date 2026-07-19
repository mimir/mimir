#include "shader_compile.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace vulkan_runner {

namespace {

namespace fs = std::filesystem;

void run_or_throw(const std::string& cmd) {
    int status = std::system(cmd.c_str());
    if (status != 0) throw std::runtime_error("command failed (" + std::to_string(status) + "): " + cmd);
}

std::vector<uint32_t> read_binary_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("could not open compiled SPIR-V file: " + path.string());
    auto size = static_cast<size_t>(in.tellg());
    if (size % 4 != 0) throw std::runtime_error("SPIR-V binary size not a multiple of 4 bytes: " + path.string());
    std::vector<uint32_t> words(size / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
    return words;
}

} // namespace

std::vector<uint32_t> compile_shader(const std::string& mim_bin, const std::string& plugin_dir,
                                      const std::string& shader_path) {
    fs::path tmp_dir = fs::temp_directory_path();
    std::string tag  = std::to_string(::getpid()) + "_" + fs::path(shader_path).stem().string();
    fs::path asm_path = tmp_dir / ("vulkan_runner_" + tag + ".spvasm");
    fs::path spv_path = tmp_dir / ("vulkan_runner_" + tag + ".spv");

    run_or_throw("\"" + mim_bin + "\" -P \"" + plugin_dir + "\" --output-spirv \"" + asm_path.string() + "\" \"" +
                 shader_path + "\"");
    run_or_throw("spirv-as --target-env vulkan1.0 \"" + asm_path.string() + "\" -o \"" + spv_path.string() + "\"");

    auto spirv = read_binary_file(spv_path);

    std::error_code ec;
    fs::remove(asm_path, ec);
    fs::remove(spv_path, ec);

    return spirv;
}

} // namespace vulkan_runner
