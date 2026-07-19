#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vulkan_runner {

/// A triangle mesh with an interleaved (position.xyz, normal.xyz) vertex
/// buffer and a matching triangle-list index buffer.
struct Mesh {
    std::vector<float> vertices; ///< 6 floats per vertex: px,py,pz,nx,ny,nz
    std::vector<uint32_t> indices;
};

/// Minimal loader for triangulated OBJ files with `v`/`vn`/`f` lines (`vt` is
/// parsed and discarded). Expects `f` lines in `v/vt/vn v/vt/vn v/vt/vn`
/// format (one triangle per face, matching `utah_teapot.obj`).
Mesh load_obj(const std::string& path);

} // namespace vulkan_runner
