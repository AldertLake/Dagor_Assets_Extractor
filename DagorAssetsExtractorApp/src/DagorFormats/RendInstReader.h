#pragma once
#include "DagorIO/BinaryReader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

enum class UvTransform : uint8_t {
    None = 0,
    ObjShortVFlipped = 1,
    ObjFloatVNegated = 2,
};

struct Vertex {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    UvTransform uvTransform = UvTransform::None;
};

struct RendInstPrimitive {
    uint32_t materialIndex = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

struct RendInstMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RendInstPrimitive> primitives;
};

class RendInstReader {
public:
    explicit RendInstReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<RendInstMesh>& GetLods() const { return m_lods; }

    // Returns the size of the container header that was stripped before the mesh data.
    // Only valid after a successful Parse() of a real GRP payload.
    size_t GetContainerHeaderSize() const { return m_containerHeaderSize; }

private:
    // Parse a real GRP RendInst payload with container/compression wrapper
    void ParseRealGrpPayload();

    std::span<const uint8_t> m_data;
    std::vector<RendInstMesh> m_lods;
    std::vector<uint8_t> m_decompressedPayload;
    size_t m_containerHeaderSize = 0;
};

} // namespace DagorFormats
