#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vulkan_runner {

/// Compiles a `.mim` shader source file into a single SPIR-V binary module
/// by shelling out to the `mim` CLI (`--output-spirv`, producing textual
/// assembly) and then `spirv-as` (assembling that text into binary) --
/// `mim`'s own binary SPIR-V emitter is still a stub, so this bridge is the
/// practical path to a real `.spv` module. All entry points declared in the
/// source file (e.g. both "vertex" and "fragment") end up in the one
/// returned module; select between them per-stage via
/// `VkPipelineShaderStageCreateInfo::pName`.
std::vector<uint32_t> compile_shader(const std::string& mim_bin, const std::string& plugin_dir,
                                      const std::string& shader_path);

} // namespace vulkan_runner
