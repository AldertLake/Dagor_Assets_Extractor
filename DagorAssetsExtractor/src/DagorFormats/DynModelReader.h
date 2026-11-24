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
    uint16_t jointIndices[4];
    float jointWeights[4];
    UvTransform uvTransform = UvTransform::None;
};

struct DynModelMesh {
    std::string name;
    std::string parentNodeName;
    bool useSkinning = false;
    std::vector<SkinnedVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RendInstPrimitive> primitives;
    std::vector<std::string> skinJointNodeNames;
    std::vector<std::array<float, 16>> skinInverseBindTransforms;
    std::vector<std::vector<uint16_t>> skinMaterialPalettes;
};

class DynModelReader {
public:
    explicit DynModelReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<DynModelMesh>& GetLods() const { return m_lods; }
    const std::vector<InternalMaterial>& GetInternalMaterials() const { return m_internalMaterials; }
    const std::vector<std::string>& GetTextureNames() const { return m_internalTextures; }
    void SetExternalTextureNames(std::vector<std::string> textureNames) { m_externalTextures = std::move(textureNames); }
    const std::vector<std::string>& GetDiagnostics() const { return m_diagnostics; }
    bool UsesExternalMaterialTextureCounts() const { return m_usesExternalMaterialTextureCounts; }

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
    bool m_usesExternalMaterialTextureCounts = false;
    std::vector<InternalMaterial> m_internalMaterials;
    std::vector<std::string> m_internalTextures;
    std::vector<std::string> m_externalTextures;
};

} // namespace DagorFormats
