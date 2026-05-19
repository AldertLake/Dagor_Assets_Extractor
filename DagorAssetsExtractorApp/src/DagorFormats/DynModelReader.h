#pragma once
#include "DagorIO/BinaryReader.h"
#include "RendInstReader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct SkinnedVertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    uint8_t jointIndices[4];
    float jointWeights[4];
};

struct DynModelMesh {
    std::string name;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RendInstPrimitive> primitives;
};

class DynModelReader {
public:
    explicit DynModelReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<DynModelMesh>& GetLods() const { return m_lods; }

private:
    std::span<const uint8_t> m_data;
    std::vector<DynModelMesh> m_lods;
};

} // namespace DagorFormats
