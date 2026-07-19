#include "obj_loader.h"

#include <array>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace vulkan_runner {

namespace {

struct VertKey {
    int v, vn;
    bool operator<(const VertKey& o) const { return v != o.v ? v < o.v : vn < o.vn; }
};

/// Parses one `v/vt/vn` face-vertex token, returning (v_idx, vn_idx), both
/// 1-based as written in the file. `vt` is parsed and discarded.
VertKey parse_face_vertex(const std::string& tok) {
    size_t first_slash  = tok.find('/');
    size_t second_slash = tok.find('/', first_slash + 1);
    int v                = std::stoi(tok.substr(0, first_slash));
    int vn               = std::stoi(tok.substr(second_slash + 1));
    return {v, vn};
}

} // namespace

Mesh load_obj(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("could not open OBJ file: " + path);

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    Mesh mesh;
    std::map<VertKey, uint32_t> seen;

    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;

        if (tag == "v") {
            std::array<float, 3> p{};
            ss >> p[0] >> p[1] >> p[2];
            positions.push_back(p);
        } else if (tag == "vn") {
            std::array<float, 3> n{};
            ss >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
        } else if (tag == "f") {
            std::string a, b, c;
            ss >> a >> b >> c;
            for (const auto& tok : {a, b, c}) {
                auto key = parse_face_vertex(tok);
                auto it  = seen.find(key);
                if (it != seen.end()) {
                    mesh.indices.push_back(it->second);
                    continue;
                }
                uint32_t idx = static_cast<uint32_t>(mesh.vertices.size() / 6);
                const auto& p = positions[key.v - 1];
                const auto& n = normals[key.vn - 1];
                mesh.vertices.insert(mesh.vertices.end(), {p[0], p[1], p[2], n[0], n[1], n[2]});
                seen[key] = idx;
                mesh.indices.push_back(idx);
            }
        }
    }

    if (mesh.indices.empty()) throw std::runtime_error("no triangles parsed from OBJ file: " + path);
    return mesh;
}

} // namespace vulkan_runner
