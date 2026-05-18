#include "RendInstReader.h"
#include <stdexcept>

namespace DagorFormats {

RendInstReader::RendInstReader(std::span<const uint8_t> data) : m_data(data) {}

void RendInstReader::Parse() {
    DagorIO::MemoryReader reader(m_data);

    if (reader.GetRemaining() < 8) {
        throw std::runtime_error("RendInst data too small");
    }

    // Check magic
    std::string magic = reader.ReadString(4);
    if (magic != "RIND") {
        throw std::runtime_error("Invalid RendInst magic: " + magic + " (complex binary format not fully decoded)");
    }

    uint32_t version = reader.ReadUInt32();
    if (version != 1) {
        throw std::runtime_error("Unsupported RendInst version: " + std::to_string(version));
    }

    uint32_t lodCount = reader.ReadUInt32();
    if (static_cast<uint64_t>(lodCount) * 40 > reader.GetRemaining()) {
        throw std::runtime_error("RendInst lodCount is unreasonably large");
    }
    m_lods.clear();
    m_lods.reserve(lodCount);

    for (uint32_t lodIdx = 0; lodIdx < lodCount; ++lodIdx) {
        if (reader.GetRemaining() < 40) { // minimum space for name (32 bytes) + vertCount (4) + indexCount (4)
            throw std::runtime_error("RendInst truncated LOD header");
        }

        RendInstMesh lod;
        // Read LOD name (fixed 32 bytes, null-terminated inside if shorter)
        std::string rawName = reader.ReadString(32);
        size_t nullPos = rawName.find('\0');
        lod.name = (nullPos != std::string::npos) ? rawName.substr(0, nullPos) : rawName;

        uint32_t vertCount = reader.ReadUInt32();
        uint32_t indexCount = reader.ReadUInt32();

        // Safety check for vertex and index sizes
        uint64_t requiredBytes = static_cast<uint64_t>(vertCount) * sizeof(Vertex) + static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
        if (requiredBytes > reader.GetRemaining()) {
            throw std::runtime_error("RendInst truncated LOD vertex/index buffers");
        }

        lod.vertices.reserve(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) {
            Vertex v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            v.z = reader.ReadFloat();
            v.nx = reader.ReadFloat();
            v.ny = reader.ReadFloat();
            v.nz = reader.ReadFloat();
            v.u = reader.ReadFloat();
            v.v = reader.ReadFloat();
            lod.vertices.push_back(v);
        }

        lod.indices.reserve(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) {
            lod.indices.push_back(reader.ReadUInt32());
        }

        // Read primitives (material slots)
        if (reader.GetRemaining() >= 4) {
            uint32_t primitiveCount = reader.ReadUInt32();
            if (static_cast<uint64_t>(primitiveCount) * 12 <= reader.GetRemaining()) { // materialIndex(4) + firstIndex(4) + indexCount(4)
                lod.primitives.reserve(primitiveCount);
                for (uint32_t p = 0; p < primitiveCount; ++p) {
                    RendInstPrimitive prim;
                    prim.materialIndex = reader.ReadUInt32();
                    prim.firstIndex = reader.ReadUInt32();
                    prim.indexCount = reader.ReadUInt32();
                    lod.primitives.push_back(prim);
                }
            }
        }

        // If no primitives were found, create a default one covering all indices
        if (lod.primitives.empty()) {
            lod.primitives.push_back({ 0, 0, indexCount });
        }

        m_lods.push_back(std::move(lod));
    }
}

} // namespace DagorFormats

