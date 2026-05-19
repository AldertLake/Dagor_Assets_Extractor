#include "DynModelReader.h"
#include "RendInstReader.h"
#include <stdexcept>

namespace DagorFormats {

DynModelReader::DynModelReader(std::span<const uint8_t> data) : m_data(data) {}

void DynModelReader::Parse() {
    DagorIO::MemoryReader reader(m_data);

    if (reader.GetRemaining() < 8) {
        throw std::runtime_error("DynModel data too small");
    }

    // Check magic
    std::string magic = reader.ReadString(4);
    if (magic != "DYN3" && magic != "DYN2") {
        throw std::runtime_error("Invalid DynModel magic: " + magic);
    }

    uint32_t version = reader.ReadUInt32();
    // Assuming version 1 or 2 for now
    if (version == 0) {
         throw std::runtime_error("Unsupported DynModel version: " + std::to_string(version));
    }

    uint32_t lodCount = reader.ReadUInt32();
    if (static_cast<uint64_t>(lodCount) * 40 > reader.GetRemaining()) {
        throw std::runtime_error("DynModel lodCount is unreasonably large");
    }
    m_lods.clear();
    m_lods.reserve(lodCount);

    for (uint32_t lodIdx = 0; lodIdx < lodCount; ++lodIdx) {
        if (reader.GetRemaining() < 40) {
            throw std::runtime_error("DynModel truncated LOD header");
        }

        DynModelMesh lod;
        // Read LOD name (fixed 32 bytes)
        std::string rawName = reader.ReadString(32);
        size_t nullPos = rawName.find('\0');
        lod.name = (nullPos != std::string::npos) ? rawName.substr(0, nullPos) : rawName;

        uint32_t vertCount = reader.ReadUInt32();
        uint32_t indexCount = reader.ReadUInt32();

        // Safety check for vertex and index sizes
        // SkinnedVertex: 3xfloat(Pos) + 3xfloat(Norm) + 2xfloat(UV) + 4xuint8(Indices) + 4xfloat(Weights)
        // Size = 12 + 12 + 8 + 4 + 16 = 52 bytes
        uint64_t requiredBytes = static_cast<uint64_t>(vertCount) * sizeof(SkinnedVertex) + static_cast<uint64_t>(indexCount) * sizeof(uint32_t);
        if (requiredBytes > reader.GetRemaining()) {
            throw std::runtime_error("DynModel truncated LOD vertex/index buffers");
        }

        lod.vertices.reserve(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) {
            SkinnedVertex v;
            v.x = reader.ReadFloat();
            v.y = reader.ReadFloat();
            v.z = reader.ReadFloat();
            v.nx = reader.ReadFloat();
            v.ny = reader.ReadFloat();
            v.nz = reader.ReadFloat();
            v.u = reader.ReadFloat();
            v.v = reader.ReadFloat();
            
            v.jointIndices[0] = reader.ReadUInt8();
            v.jointIndices[1] = reader.ReadUInt8();
            v.jointIndices[2] = reader.ReadUInt8();
            v.jointIndices[3] = reader.ReadUInt8();

            v.jointWeights[0] = reader.ReadFloat();
            v.jointWeights[1] = reader.ReadFloat();
            v.jointWeights[2] = reader.ReadFloat();
            v.jointWeights[3] = reader.ReadFloat();

            lod.vertices.push_back(v);
        }

        lod.indices.reserve(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) {
            lod.indices.push_back(reader.ReadUInt32());
        }

        // Read primitives (material slots)
        if (reader.GetRemaining() >= 4) {
            uint32_t primitiveCount = reader.ReadUInt32();
            if (static_cast<uint64_t>(primitiveCount) * 12 <= reader.GetRemaining()) {
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

        if (lod.primitives.empty()) {
            lod.primitives.push_back({ 0, 0, indexCount });
        }

        m_lods.push_back(std::move(lod));
    }
}

} // namespace DagorFormats
