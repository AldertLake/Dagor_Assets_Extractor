#pragma once
#include "DagorIO/BinaryReader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

struct RendInstMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

class RendInstReader {
public:
    explicit RendInstReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<RendInstMesh>& GetLods() const { return m_lods; }

private:
    std::span<const uint8_t> m_data;
    std::vector<RendInstMesh> m_lods;
};

} // namespace DagorFormats
