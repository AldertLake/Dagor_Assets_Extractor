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
    std::string parentNodeName;
    bool useSkinning = false;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RendInstPrimitive> primitives;
};

class DynModelReader {
public:
    explicit DynModelReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<DynModelMesh>& GetLods() const { return m_lods; }
    const std::vector<std::string>& GetDiagnostics() const { return m_diagnostics; }

    // Returns the size of the container header that was stripped before the mesh data.
    // Only valid after a successful Parse() of a real GRP payload.
    size_t GetContainerHeaderSize() const { return m_containerHeaderSize; }

private:
    // Parse the synthetic test format (DYN2/DYN3 magic)
    void ParseSynthetic();

    // Parse a real GRP DynModel payload with container/compression wrapper
    void ParseRealGrpPayload();
    void AddDiagnostic(std::string message);

    std::span<const uint8_t> m_data;
    std::vector<DynModelMesh> m_lods;
    std::vector<uint8_t> m_decompressedPayload;
    std::vector<std::string> m_diagnostics;
    size_t m_containerHeaderSize = 0;
};

} // namespace DagorFormats
