#include "DynModelReader.h"
#include "RendInstReader.h"
#include "DagorIO/Decompression.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <array>
#include <map>
#include <unordered_map>
#include <limits>
#include <cmath>
#include <iomanip>
#include <utility>

namespace DagorFormats {

namespace {

    constexpr uint32_t kBlockSizeMask = 0x3FFFFFFFu;
    constexpr unsigned kBlockCompressionNone = 0;
    constexpr unsigned kBlockCompressionZstd = 1;
    constexpr unsigned kBlockCompressionOodle = 2;
    constexpr uint32_t kLodHeaderSizeMask = 0x00FFFFFFu;
    constexpr uint32_t kLodHeaderExtendedRiStructFlag = 0x01000000u;
    constexpr uint32_t kMvdHeaderSizeMask = 0x3FFFFFFFu;
    constexpr uint32_t kExternalMaterialTextureCount = 0xFFFFFFFFu;
    constexpr uint32_t kDefaultRiStructSize = 0xA0u;
    constexpr uint32_t kExtendedRiStructSizeAdd = 184u;
    constexpr uint32_t kDefaultRiStructSizeAdd = 48u;
    constexpr uint32_t kMaxReasonableCount = 1'000'000u;
    constexpr uint32_t kMaxVertexCount = 5'000'000u;
    constexpr uint32_t kMaxDecodedIndices = 6'000'000u;
    constexpr size_t kTextureMapHeaderSize = 0x14u;
    constexpr size_t kTextureIndexOffsetBias = 0x18u;
    constexpr uint32_t kTextureNameOffsetBias = 0x10u;

    struct DynModelPhase1Header
    {
        uint32_t lodHdrInfo = 0;
        uint32_t lodHdrSize = 0;
        uint32_t riStructSize = 0;
        uint32_t texCnt = 0;
        uint32_t matCnt = 0;
        uint32_t vdataNum = 0;
        uint32_t mvdHdrPacked = 0;
        uint32_t mvdHdrSize = 0;
        bool mvdHdrFlag = false;
        bool externalMaterialTextureCounts = false;
    };

    struct GlobalVertexData
    {
        uint32_t vCnt = 0;
        uint8_t vStride = 0;
        uint32_t iPackedSz = 0;
        uint32_t iSz = 0;
        uint16_t flags = 0;
        uint16_t bf_14 = 0;
        uint32_t iCnt = 0;
        uint32_t storageFormat = 0;
    };

    constexpr int MVD_NORMAL = 0;
    constexpr int MVD_SKINNED = 1;
    constexpr int MVD_SKINNED_FLAG = 2;

    void ReadShortUv(DagorIO::MemoryReader& reader, SkinnedVertex& vertex)
    {
        vertex.u = static_cast<float>(reader.ReadInt16()) / 4096.0f;
        vertex.v = 1.0f - (static_cast<float>(reader.ReadInt16()) / 4096.0f);
        vertex.uvTransform = UvTransform::ObjShortVFlipped;
    }

    void ReadFloatUv(DagorIO::MemoryReader& reader, SkinnedVertex& vertex)
    {
        vertex.u = reader.ReadFloat();
        vertex.v = -reader.ReadFloat();
        vertex.uvTransform = UvTransform::ObjFloatVNegated;
    }

    uint32_t ReadU32(std::span<const uint8_t> data, size_t offset)
    {
        if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
        {
            throw std::runtime_error("DynModel read out of bounds");
        }
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    }

    bool CheckedAdd(size_t lhs, size_t rhs, size_t& out)
    {
        if (rhs > std::numeric_limits<size_t>::max() - lhs)
        {
            return false;
        }
        out = lhs + rhs;
        return true;
    }

    bool CheckedMul(size_t lhs, size_t rhs, size_t& out)
    {
        if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
        {
            return false;
        }
        out = lhs * rhs;
        return true;
    }

    bool IsRangeWithin(size_t offset, size_t size, size_t total)
    {
        return offset <= total && size <= total - offset;
    }

    bool LooksLikeZlibHeader(std::span<const uint8_t> data)
    {
        if (data.size() < 2)
        {
            return false;
        }

        const uint8_t cmf = data[0];
        const uint8_t flg = data[1];
        const uint16_t header = (static_cast<uint16_t>(cmf) << 8) | flg;
        return (cmf & 0x0Fu) == 8u &&
               (cmf >> 4) <= 7u &&
               (header % 31u) == 0u;
    }

    std::string HexPrefix(std::span<const uint8_t> data, size_t maxBytes)
    {
        std::ostringstream out;
        out << std::hex << std::uppercase << std::setfill('0');
        const size_t count = std::min(data.size(), maxBytes);
        for (size_t i = 0; i < count; ++i)
        {
            if (i != 0)
            {
                out << ' ';
            }
            out << std::setw(2) << static_cast<unsigned int>(data[i]);
        }
        return out.str();
    }

    bool HasSyntheticMagic(std::span<const uint8_t> data)
    {
        if (data.size() < 4) return false;
        return (std::memcmp(data.data(), "DYN3", 4) == 0 ||
                std::memcmp(data.data(), "DYN2", 4) == 0);
    }

    std::string TrimDagorString(std::span<const uint8_t> data)
    {
        size_t end = data.size();
        while (end > 0 && data[end - 1] == 0)
        {
            --end;
        }

        size_t firstNull = 0;
        while (firstNull < end && data[firstNull] != 0)
        {
            ++firstNull;
        }

        return std::string(
            reinterpret_cast<const char*>(data.data()),
            reinterpret_cast<const char*>(data.data() + firstNull));
    }

    DynModelPhase1Header ReadDynModelPhase1Header(DagorIO::MemoryReader& reader)
    {
        if (reader.GetRemaining() < sizeof(uint32_t) * 5)
        {
            throw std::runtime_error("DynModel ShaderMeshResource header is truncated");
        }

        DynModelPhase1Header header;
        header.lodHdrInfo = reader.ReadUInt32();
        header.lodHdrSize = header.lodHdrInfo & kLodHeaderSizeMask;

        const uint32_t riStructSizeAdd =
            (header.lodHdrInfo & kLodHeaderExtendedRiStructFlag) != 0
                ? kExtendedRiStructSizeAdd
                : kDefaultRiStructSizeAdd;
        const uint32_t alternateRiStructSize = header.lodHdrSize + riStructSizeAdd;
        header.riStructSize = alternateRiStructSize > kDefaultRiStructSize
            ? alternateRiStructSize
            : kDefaultRiStructSize;

        header.texCnt = reader.ReadUInt32();
        header.matCnt = reader.ReadUInt32();
        header.vdataNum = reader.ReadUInt32();
        header.mvdHdrPacked = reader.ReadUInt32();
        header.mvdHdrSize = header.mvdHdrPacked & kMvdHeaderSizeMask;
        header.mvdHdrFlag = header.mvdHdrPacked != header.mvdHdrSize;
        header.externalMaterialTextureCounts =
            header.texCnt == kExternalMaterialTextureCount &&
            header.matCnt == kExternalMaterialTextureCount;

        return header;
    }

    std::vector<std::string> ReadDynModelTextureNames(
        DagorIO::MemoryReader& reader,
        uint32_t headerTextureCount)
    {
        if (headerTextureCount == 0)
        {
            return {};
        }

        if (reader.GetRemaining() < kTextureMapHeaderSize)
        {
            throw std::runtime_error("DynModel texture map header is truncated");
        }

        [[maybe_unused]] const uint32_t texMapSize = reader.ReadUInt32();
        const uint32_t texIndicesOfs = reader.ReadUInt32();
        const uint32_t textureNameCount = reader.ReadUInt32();
        reader.Skip(8);

        if (textureNameCount > kMaxReasonableCount)
        {
            throw std::runtime_error("DynModel texture name count is unreasonable");
        }

        const size_t stringBlobStart = reader.GetPosition();
        const uint64_t indexTablePos64 =
            static_cast<uint64_t>(texIndicesOfs) + kTextureIndexOffsetBias;
        if (indexTablePos64 > reader.GetSize() || indexTablePos64 < stringBlobStart)
        {
            std::cerr << "DEBUG: DynModelReader tex error. reader.GetSize()=" << reader.GetSize() 
                      << " texIndicesOfs=" << texIndicesOfs 
                      << " indexTablePos64=" << indexTablePos64 
                      << " stringBlobStart=" << stringBlobStart 
                      << " texCnt=" << headerTextureCount 
                      << std::endl;
            throw std::runtime_error("DynModel texture name index table points outside the payload");
        }

        const size_t indexTablePos = static_cast<size_t>(indexTablePos64);
        const size_t stringBlobSize = indexTablePos - stringBlobStart;
        std::span<const uint8_t> stringBlob = reader.ReadSpan(stringBlobSize);

        size_t indexBytes = 0;
        if (!CheckedMul(static_cast<size_t>(textureNameCount), sizeof(uint32_t), indexBytes) ||
            indexBytes > reader.GetRemaining())
        {
            throw std::runtime_error("DynModel texture name index table is truncated");
        }

        std::vector<uint32_t> offsets;
        offsets.reserve(textureNameCount);
        for (uint32_t textureIndex = 0; textureIndex < textureNameCount; ++textureIndex)
        {
            const uint32_t rawOffset = reader.ReadUInt32();
            const uint32_t adjustedOffset = rawOffset - kTextureNameOffsetBias;
            offsets.push_back(adjustedOffset);
        }

        std::vector<std::string> textureNames;
        textureNames.reserve(textureNameCount);
        for (uint32_t textureIndex = 0; textureIndex < textureNameCount; ++textureIndex)
        {
            const size_t begin = offsets[textureIndex];
            const size_t end = (textureIndex + 1 < textureNameCount)
                ? offsets[textureIndex + 1]
                : stringBlob.size();

            if (begin >= stringBlob.size() || end > stringBlob.size() || begin > end)
            {
                textureNames.push_back("");
                continue;
            }
            textureNames.push_back(TrimDagorString(stringBlob.subspan(begin, end - begin)));
        }

        return textureNames;
    }

    void LogDynModelPhase1Header(
        const DynModelPhase1Header& header,
        const std::vector<std::string>& textureNames,
        size_t decompressedSize,
        size_t containerHeaderSize)
    {
        std::ostringstream message;
        message << "DynModel Phase 1: decompressed " << decompressedSize
                << " bytes of ShaderMeshResource data"
                << " (container header " << containerHeaderSize << " bytes). "
                << "lodHdrInfo=0x" << std::hex << header.lodHdrInfo << std::dec
                << ", lodHdrSize=" << header.lodHdrSize
                << ", riStructSize=" << header.riStructSize
                << ", texCnt=";

        if (header.externalMaterialTextureCounts)
            message << "external";
        else
            message << header.texCnt;

        message << ", matCnt=";
        if (header.externalMaterialTextureCounts)
            message << "external";
        else
            message << header.matCnt;

        message << ", vdataNum=" << header.vdataNum
                << ", mvdHdrSize=" << header.mvdHdrSize
                << ", mvdHdrFlag=" << (header.mvdHdrFlag ? "true" : "false")
                << ", textureNamesRead=" << textureNames.size();

        std::cout << message.str() << std::endl;
    }

    std::vector<InternalMaterial> ParseInternalMaterials(
        std::span<const uint8_t> mvdPayload,
        const std::vector<std::string>& textureNames)
    {
        if (mvdPayload.size() < 24)
        {
            return {};
        }

        DagorIO::MemoryReader reader(mvdPayload);
        const uint32_t matOfs = reader.ReadUInt32();
        const uint32_t matCnt = reader.ReadUInt32();

        if (matOfs == 0 || matCnt == 0)
        {
            return {};
        }

        if (matCnt > 10000)
        {
            return {};
        }

        std::vector<InternalMaterial> materials;
        materials.reserve(matCnt);

        for (uint32_t idx = 0; idx < matCnt; ++idx)
        {
            size_t entryPos = matOfs + idx * 0xA8;
            if (entryPos + 0xA8 > mvdPayload.size())
            {
                break;
            }

            reader.Seek(entryPos);
            reader.Skip(0x40); // diff, amb, emis, spec
            reader.Skip(4);    // unknown
            uint32_t shaderOfs = reader.ReadUInt32();
            reader.Skip(8);    // padding

            uint32_t textureIds[16];
            for (int i = 0; i < 16; ++i)
            {
                textureIds[i] = reader.ReadUInt32();
            }

            std::string shaderClass;
            if (shaderOfs < mvdPayload.size())
            {
                size_t sOfs = shaderOfs;
                while (sOfs < mvdPayload.size() && mvdPayload[sOfs] != '\0')
                {
                    shaderClass.push_back(static_cast<char>(mvdPayload[sOfs]));
                    sOfs++;
                }
            }

            InternalMaterial mat;
            mat.shaderClass = shaderClass;
            for (int i = 0; i < 16; ++i)
            {
                uint32_t texId = textureIds[i];
                if (texId != 0xFFFFFFFF && texId < textureNames.size())
                {
                    mat.textureSlots[i] = textureNames[texId];
                }
            }
            materials.push_back(mat);
        }

        return materials;
    }

} // namespace

DynModelReader::DynModelReader(std::span<const uint8_t> data) : m_data(data) {}

void DynModelReader::AddDiagnostic(std::string message)
{
    m_diagnostics.push_back(std::move(message));
}

void DynModelReader::Parse() {
    m_diagnostics.clear();
    if (HasSyntheticMagic(m_data))
    {
        ParseSynthetic();
        return;
    }
    ParseRealGrpPayload();
}

void DynModelReader::ParseSynthetic() {
    DagorIO::MemoryReader reader(m_data);
    reader.Skip(4); // magic
    reader.ReadUInt32(); // version
    uint32_t lodCount = reader.ReadUInt32();
    m_lods.clear();
    m_lods.reserve(lodCount);

    for (uint32_t lodIdx = 0; lodIdx < lodCount; ++lodIdx) {
        DynModelMesh lod;
        lod.useSkinning = true;
        std::string rawName = reader.ReadString(32);
        size_t nullPos = rawName.find('\0');
        lod.name = (nullPos != std::string::npos) ? rawName.substr(0, nullPos) : rawName;

        uint32_t vertCount = reader.ReadUInt32();
        uint32_t indexCount = reader.ReadUInt32();

        lod.vertices.reserve(vertCount);
        for (uint32_t i = 0; i < vertCount; ++i) {
            SkinnedVertex v;
            v.x = reader.ReadFloat(); v.y = reader.ReadFloat(); v.z = reader.ReadFloat();
            v.nx = reader.ReadFloat(); v.ny = reader.ReadFloat(); v.nz = reader.ReadFloat();
            v.u = reader.ReadFloat(); v.v = reader.ReadFloat();
            v.jointIndices[0] = reader.ReadUInt8(); v.jointIndices[1] = reader.ReadUInt8();
            v.jointIndices[2] = reader.ReadUInt8(); v.jointIndices[3] = reader.ReadUInt8();
            v.jointWeights[0] = reader.ReadFloat(); v.jointWeights[1] = reader.ReadFloat();
            v.jointWeights[2] = reader.ReadFloat(); v.jointWeights[3] = reader.ReadFloat();
            lod.vertices.push_back(v);
        }

        lod.indices.reserve(indexCount);
        for (uint32_t i = 0; i < indexCount; ++i) lod.indices.push_back(reader.ReadUInt32());

        if (reader.GetRemaining() >= 4) {
            uint32_t primitiveCount = reader.ReadUInt32();
            for (uint32_t p = 0; p < primitiveCount; ++p) {
                RendInstPrimitive prim;
                prim.materialIndex = reader.ReadUInt32();
                prim.firstIndex = reader.ReadUInt32();
                prim.indexCount = reader.ReadUInt32();
                lod.primitives.push_back(prim);
            }
        }
        if (lod.primitives.empty()) lod.primitives.push_back({ 0, 0, indexCount });
        m_lods.push_back(std::move(lod));
    }
}

void DynModelReader::ParseRealGrpPayload() {
    struct CompressedBlock
    {
        size_t rawSize = 0;
        uint8_t method = 0;
        std::vector<uint8_t> decompressed;
    };

    struct DecodedVertexData
    {
        uint32_t lodId = 0;
        std::vector<SkinnedVertex> vertices;
        std::vector<uint32_t> indices;
    };

    struct ShaderMeshElem
    {
        uint32_t materialIndex = 0;
        uint32_t vertexDataIndex = 0;
        uint32_t vdOrderIndex = 0;
        uint32_t startIndex = 0;
        uint32_t faceCount = 0;
        uint32_t baseVertex = 0;
    };

    struct ShaderMesh
    {
        std::vector<ShaderMeshElem> elements;
    };

    struct RigidObject
    {
        uint32_t nodeId = 0;
    };

    struct LodInfo
    {
        uint32_t lodId = 0;
        uint32_t skinCount = 0;
        std::vector<RigidObject> rigidObjects;
        std::vector<ShaderMesh> rigidMeshes;
    };

    struct SkinnedLod
    {
        std::vector<uint32_t> skinNodes;
        std::vector<ShaderMesh> meshes;
    };

    auto readU24 = [](std::span<const uint8_t> data) -> uint32_t
    {
        if (data.size() < 3)
        {
            throw std::runtime_error("DynModel compressed block header is truncated");
        }

        return static_cast<uint32_t>(data[0]) |
               (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16);
    };

    auto decompressBlock = [&](std::span<const uint8_t> data) -> CompressedBlock
    {
        if (data.size() < 4)
        {
            throw std::runtime_error("DynModel compressed block is truncated");
        }

        CompressedBlock block;
        const uint32_t compressedSize = readU24(data);
        block.method = data[3];
        block.rawSize = 4ull + compressedSize;

        if (data.size() < block.rawSize)
        {
            throw std::runtime_error("DynModel compressed block exceeds payload bounds");
        }

        const std::span<const uint8_t> payload = data.subspan(4, compressedSize);
        switch (block.method)
        {
        case 0x40:
            block.decompressed = DagorIO::Decompressor::DecompressZstd(payload);
            break;
        case 0x60:
            block.decompressed = DagorIO::Decompressor::DecompressZlib(payload);
            break;
        case 0x80:
            if (payload.size() < sizeof(uint32_t))
            {
                throw std::runtime_error("DynModel Oodle block is truncated");
            }
            block.decompressed = DagorIO::Decompressor::DecompressOodle(
                payload.subspan(sizeof(uint32_t)),
                ReadU32(payload, 0),
                "",
                "");
            break;
        case 0x20:
            block.decompressed = DagorIO::Decompressor::DecompressLzma(payload);
            break;
        default:
            throw std::runtime_error("DynModel uses an unknown compressed block method: " + std::to_string(block.method));
        }

        return block;
    };

    auto readShaderMesh = [&](DagorIO::MemoryReader& reader) -> ShaderMesh
    {
        if (reader.GetRemaining() < 0x20)
        {
            throw std::runtime_error("DynModel ShaderMesh header is truncated");
        }

        ShaderMesh mesh;
        reader.ReadUInt32();
        uint32_t count = reader.ReadUInt32();
        reader.Skip(8);

        std::array<uint16_t, 8> stageEndElemIdx = {};
        for (uint16_t& value : stageEndElemIdx)
        {
            value = reader.ReadUInt16();
        }

        int32_t deprecatedMaxMatPass = reader.ReadInt32();
        reader.ReadUInt32();

        if (deprecatedMaxMatPass >= 0 && stageEndElemIdx[2] != 0)
        {
            count += stageEndElemIdx[2];
        }

        if (count > kMaxReasonableCount)
        {
            throw std::runtime_error("DynModel ShaderMesh element count is unreasonable");
        }

        mesh.elements.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            ShaderMeshElem element;
            reader.ReadUInt64();
            element.materialIndex = static_cast<uint32_t>(reader.ReadUInt64());
            element.vertexDataIndex = static_cast<uint32_t>(reader.ReadUInt64());
            element.vdOrderIndex = reader.ReadUInt32();
            reader.ReadUInt32();
            reader.ReadUInt32();
            element.startIndex = reader.ReadUInt32();
            element.faceCount = reader.ReadUInt32();
            element.baseVertex = reader.ReadUInt32();
            mesh.elements.push_back(element);
        }

        return mesh;
    };

    auto decodeMatVData = [&](std::span<const uint8_t> payload, int mvdFlag) -> std::vector<DecodedVertexData>
    {
        DagorIO::MemoryReader meshReader(payload);
        if (meshReader.GetRemaining() < 24)
        {
            throw std::runtime_error("DynModel MatVData header is truncated");
        }

        meshReader.ReadUInt32();
        meshReader.ReadUInt32();
        meshReader.Skip(8);
        const uint32_t mvdGvdOfs = meshReader.ReadUInt32();
        const uint32_t mvdGvdCnt = meshReader.ReadUInt32();

        {
            std::ostringstream diag;
            diag << "DynModel MatVData: payload=" << payload.size()
                 << " bytes, gvdOfs=" << mvdGvdOfs
                 << ", gvdCnt=" << mvdGvdCnt
                 << ", mvdFlag=" << mvdFlag;
            AddDiagnostic(diag.str());
        }

        if (mvdGvdCnt > kMaxReasonableCount)
        {
            throw std::runtime_error("DynModel MatVData GVD count is unreasonable: " + std::to_string(mvdGvdCnt));
        }

        size_t gvdTableBytes = 0;
        size_t gvdTableEnd = 0;
        if (!CheckedMul(static_cast<size_t>(mvdGvdCnt), static_cast<size_t>(0x20), gvdTableBytes) ||
            !CheckedAdd(static_cast<size_t>(mvdGvdOfs), gvdTableBytes, gvdTableEnd) ||
            !IsRangeWithin(static_cast<size_t>(mvdGvdOfs), gvdTableBytes, payload.size()))
        {
            throw std::runtime_error("DynModel MatVData GVD table is out of bounds");
        }

        if (mvdGvdOfs > 0)
        {
            meshReader.Seek(mvdGvdOfs);
        }

        std::vector<GlobalVertexData> gvDataList;
        gvDataList.reserve(mvdGvdCnt);

        for (uint32_t i = 0; i < mvdGvdCnt; ++i)
        {
            GlobalVertexData gvd;
            gvd.vCnt = meshReader.ReadUInt32();
            const uint32_t packed1 = meshReader.ReadUInt32();
            gvd.vStride = static_cast<uint8_t>(packed1 & 0xFF);
            gvd.iPackedSz = packed1 >> 8;
            gvd.iSz = meshReader.ReadUInt32();
            const uint32_t packed2 = meshReader.ReadUInt32();
            gvd.flags = static_cast<uint16_t>(packed2 & 0xFFFF);
            gvd.bf_14 = static_cast<uint16_t>(packed2 >> 16);
            gvd.iCnt = meshReader.ReadUInt32();
            gvd.storageFormat = meshReader.ReadUInt32() + (mvdFlag * 10);
            meshReader.Skip(8);

            if (gvd.vStride == 0)
            {
                throw std::runtime_error("DynModel GVD has zero vertex stride");
            }
            if (gvd.vCnt > kMaxVertexCount)
            {
                throw std::runtime_error("DynModel GVD vertex count is unreasonable: " + std::to_string(gvd.vCnt));
            }
            if (gvd.iCnt > kMaxDecodedIndices)
            {
                throw std::runtime_error("DynModel GVD index count is unreasonable: " + std::to_string(gvd.iCnt));
            }
            if (gvd.iPackedSz > 0 && gvd.iPackedSz < 5)
            {
                throw std::runtime_error("DynModel packed index stream is too small");
            }
            if (gvd.iPackedSz == 0 && (gvd.iSz % sizeof(uint16_t)) != 0)
            {
                throw std::runtime_error("DynModel unpacked index stream has an odd byte size");
            }

            if (i < 16)
            {
                std::ostringstream diag;
                diag << "DynModel GVD[" << i << "]: vCnt=" << gvd.vCnt
                     << ", vStride=" << static_cast<uint32_t>(gvd.vStride)
                     << ", iPackedSz=" << gvd.iPackedSz
                     << ", iSz=" << gvd.iSz
                     << ", iCnt=" << gvd.iCnt
                     << ", flags=0x" << std::hex << gvd.flags << std::dec
                     << ", storageFormat=" << gvd.storageFormat;
                AddDiagnostic(diag.str());
            }
            else if (i == 16)
            {
                AddDiagnostic("DynModel GVD diagnostics truncated after 16 entries.");
            }

            gvDataList.push_back(gvd);
        }

        const size_t vdStartOfs = mvdGvdOfs + (static_cast<size_t>(mvdGvdCnt) * 0x20);
        meshReader.Seek(vdStartOfs);

        std::vector<DecodedVertexData> decodedData;
        decodedData.reserve(mvdGvdCnt);

        for (size_t gvdIndex = 0; gvdIndex < gvDataList.size(); ++gvdIndex)
        {
            const auto& gvd = gvDataList[gvdIndex];
            const size_t blockStartPos = meshReader.GetPosition();
            size_t vbSz = 0;
            const size_t ibSz = (gvd.iPackedSz == 0) ? static_cast<size_t>(gvd.iSz)
                                                     : static_cast<size_t>(gvd.iPackedSz);
            size_t fullBlockSz = 0;
            if (!CheckedMul(static_cast<size_t>(gvd.vCnt), static_cast<size_t>(gvd.vStride), vbSz) ||
                !CheckedAdd(vbSz, ibSz, fullBlockSz) ||
                !IsRangeWithin(blockStartPos, fullBlockSz, payload.size()))
            {
                std::ostringstream error;
                error << "DynModel GVD[" << gvdIndex << "] vertex/index block is out of bounds"
                      << " (blockStart=" << blockStartPos
                      << ", vbSz=" << vbSz
                      << ", ibSz=" << ibSz
                      << ", payload=" << payload.size() << ")";
                throw std::runtime_error(error.str());
            }

            DecodedVertexData dataBlock;
            dataBlock.lodId = (gvd.flags & 0xF000u) >> 12;
            dataBlock.vertices.reserve(gvd.vCnt);

            for (uint32_t vIdx = 0; vIdx < gvd.vCnt; ++vIdx)
            {
                const size_t vertexStartPos = meshReader.GetPosition();
                SkinnedVertex v = {};
                v.jointIndices[0] = 0;
                v.jointIndices[1] = 0;
                v.jointIndices[2] = 0;
                v.jointIndices[3] = 0;
                v.jointWeights[0] = 1.0f;

                bool decoded = false;
                const uint32_t fmt = gvd.storageFormat;
                const uint8_t stride = gvd.vStride;

                if (fmt == 1 && stride == 12) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    v.u = 0.0f; v.v = 0.0f;
                    decoded = true;
                }
                else if (fmt == 2) {
                    if (stride == 12) {
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(2);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 16) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 3) {
                    if (stride == 16) {
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 20) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 24) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadFloatUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 4) {
                    if (stride == 20) {
                        meshReader.Skip(4);
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 28) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadFloatUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 5) {
                    if (stride == 24) {
                        ReadShortUv(meshReader, v);
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        decoded = true;
                    }
                    else if (stride == 28) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(12);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 36) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadFloatUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 6) {
                    if (stride == 32) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 13) {
                    if (stride == 16) {
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 20) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 24) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadFloatUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 15) {
                    if (stride == 24) {
                        meshReader.Skip(8);
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 28) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(8);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 32) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        decoded = true;
                    }
                }
                else if (fmt == 16) {
                    if (stride == 28) {
                        meshReader.Skip(12);
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 32) {
                        meshReader.Skip(4);
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 36) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(14);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 17) {
                    if (stride == 32) {
                        meshReader.Skip(8);
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        decoded = true;
                    }
                    else if (stride == 36) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(8);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 25) {
                    if (stride == 24) {
                        v.x = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.y = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        v.z = static_cast<float>(meshReader.ReadInt16()) / 32768.0f;
                        meshReader.Skip(6);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 28) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                    else if (stride == 32) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadFloatUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 26) {
                    if (stride == 32) {
                        meshReader.Skip(4);
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }
                else if (fmt == 27) {
                    if (stride == 36) {
                        v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                        meshReader.Skip(4);
                        ReadShortUv(meshReader, v);
                        decoded = true;
                    }
                }

                if (!decoded)
                {
                    throw std::runtime_error(
                        "Unimplemented vertex storage format: " +
                        std::to_string(gvd.storageFormat) +
                        " stride: " +
                        std::to_string(gvd.vStride));
                }

                dataBlock.vertices.push_back(v);
                meshReader.Seek(vertexStartPos + gvd.vStride);
            }

            meshReader.Seek(blockStartPos + vbSz);

            if (gvd.iPackedSz == 0)
            {
                const size_t numIndices = gvd.iSz / sizeof(uint16_t);
                dataBlock.indices.reserve(numIndices);
                for (size_t i = 0; i < numIndices; i += 3)
                {
                    if (i + 2 < numIndices)
                    {
                        const uint16_t i0 = meshReader.ReadUInt16();
                        const uint16_t i1 = meshReader.ReadUInt16();
                        const uint16_t i2 = meshReader.ReadUInt16();
                        dataBlock.indices.push_back(i0);
                        dataBlock.indices.push_back(i2);
                        dataBlock.indices.push_back(i1);
                    }
                    else
                    {
                        for (size_t j = i; j < numIndices; ++j)
                        {
                            dataBlock.indices.push_back(meshReader.ReadUInt16());
                        }
                    }
                }
            }
            else
            {
                const size_t streamEnd = blockStartPos + vbSz + (gvd.iPackedSz >= 4 ? gvd.iPackedSz - 4 : 0);

                meshReader.Skip(1);
                int32_t stateBuffer[2] = { 0, 0 };
                std::vector<uint32_t> tempIndices;
                tempIndices.reserve(std::min<size_t>(
                    std::max<size_t>(static_cast<size_t>(gvd.iCnt), static_cast<size_t>(gvd.iPackedSz)),
                    static_cast<size_t>(kMaxDecodedIndices)));

                while (meshReader.GetPosition() < streamEnd)
                {
                    uint32_t decodedLeb = 0;
                    uint32_t shift = 0;
                    bool readAnyByte = false;
                    while (true)
                    {
                        if (meshReader.GetPosition() >= streamEnd)
                        {
                            break;
                        }
                        const uint8_t byte = meshReader.ReadUInt8();
                        readAnyByte = true;
                        if (shift >= 32)
                        {
                            throw std::runtime_error("DynModel packed index LEB128 value is too large");
                        }
                        decodedLeb |= (static_cast<uint32_t>(byte & 0x7F) << shift);
                        shift += 7;
                        if ((byte & 0x80) == 0)
                        {
                            break;
                        }
                    }

                    if (!readAnyByte)
                    {
                        break;
                    }

                    const uint32_t current = decodedLeb & 1;
                    const int32_t delta = (decodedLeb >> 2) ^ -(static_cast<int32_t>((decodedLeb & 2) != 0));
                    const int32_t index = stateBuffer[current] + delta;
                    if (index < 0)
                    {
                        throw std::runtime_error("DynModel packed index stream decoded a negative index");
                    }
                    stateBuffer[current] = index;
                    tempIndices.push_back(static_cast<uint32_t>(index));
                    if (tempIndices.size() > kMaxDecodedIndices)
                    {
                        throw std::runtime_error("DynModel packed index stream decoded too many indices");
                    }
                }

                dataBlock.indices.reserve(tempIndices.size());
                for (size_t i = 0; i < tempIndices.size(); i += 3)
                {
                    if (i + 2 < tempIndices.size())
                    {
                        dataBlock.indices.push_back(tempIndices[i]);
                        dataBlock.indices.push_back(tempIndices[i + 2]);
                        dataBlock.indices.push_back(tempIndices[i + 1]);
                    }
                    else
                    {
                        for (size_t j = i; j < tempIndices.size(); ++j)
                        {
                            dataBlock.indices.push_back(tempIndices[j]);
                        }
                    }
                }
            }

            if (gvdIndex < 16)
            {
                std::ostringstream diag;
                diag << "DynModel GVD[" << gvdIndex << "] decoded vertices="
                     << dataBlock.vertices.size()
                     << ", indices=" << dataBlock.indices.size()
                     << ", blockStart=" << blockStartPos
                     << ", blockSize=" << fullBlockSz;
                AddDiagnostic(diag.str());
            }

            meshReader.Seek(blockStartPos + fullBlockSz);
            decodedData.push_back(std::move(dataBlock));
        }

        return decodedData;
    };

    auto computeNormals = [](DynModelMesh& mesh)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return;
        }

        for (auto& vertex : mesh.vertices)
        {
            vertex.nx = 0.0f;
            vertex.ny = 0.0f;
            vertex.nz = 0.0f;
        }

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint32_t i0 = mesh.indices[i];
            const uint32_t i1 = mesh.indices[i + 1];
            const uint32_t i2 = mesh.indices[i + 2];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size())
            {
                continue;
            }

            const auto& a = mesh.vertices[i0];
            const auto& b = mesh.vertices[i1];
            const auto& c = mesh.vertices[i2];

            const float abx = b.x - a.x;
            const float aby = b.y - a.y;
            const float abz = b.z - a.z;
            const float acx = c.x - a.x;
            const float acy = c.y - a.y;
            const float acz = c.z - a.z;

            const float nx = (aby * acz) - (abz * acy);
            const float ny = (abz * acx) - (abx * acz);
            const float nz = (abx * acy) - (aby * acx);

            mesh.vertices[i0].nx += nx; mesh.vertices[i0].ny += ny; mesh.vertices[i0].nz += nz;
            mesh.vertices[i1].nx += nx; mesh.vertices[i1].ny += ny; mesh.vertices[i1].nz += nz;
            mesh.vertices[i2].nx += nx; mesh.vertices[i2].ny += ny; mesh.vertices[i2].nz += nz;
        }

        for (auto& vertex : mesh.vertices)
        {
            const float length = std::sqrt(
                (vertex.nx * vertex.nx) +
                (vertex.ny * vertex.ny) +
                (vertex.nz * vertex.nz));
            if (length > 1e-6f)
            {
                vertex.nx /= length;
                vertex.ny /= length;
                vertex.nz /= length;
            }
            else
            {
                vertex.nz = 1.0f;
            }
        }
    };

    auto resolveNodeName = [](const std::vector<std::string>& nodeNames,
                              const std::unordered_map<uint32_t, size_t>& skinNodeMap,
                              uint32_t nodeId) -> std::string
    {
        const auto it = skinNodeMap.find(nodeId);
        if (it == skinNodeMap.end() || it->second >= nodeNames.size())
        {
            return {};
        }
        return nodeNames[it->second];
    };

    auto assembleMesh = [&](const std::string& meshName,
                            const std::string& parentNodeName,
                            const std::vector<ShaderMeshElem>& elements,
                            const std::vector<DecodedVertexData>& vertexData,
                            const float positionScale[3],
                            const float positionOffset[3],
                            bool useSkinning,
                            size_t materialIndexOffset) -> DynModelMesh
    {
        DynModelMesh mesh;
        mesh.name = meshName;
        mesh.parentNodeName = parentNodeName;
        mesh.useSkinning = useSkinning;

        std::unordered_map<uint32_t, size_t> orderGroupIndices;
        std::vector<std::vector<const ShaderMeshElem*>> orderedGroups;
        std::unordered_map<uint64_t, uint32_t> remappedVertices;

        for (const auto& element : elements)
        {
            if (element.vertexDataIndex >= vertexData.size())
            {
                AddDiagnostic("DynModel skipped ShaderMesh element with out-of-range vertexDataIndex=" +
                    std::to_string(element.vertexDataIndex));
                continue;
            }

            const auto& source = vertexData[element.vertexDataIndex];
            if (source.vertices.empty())
            {
                AddDiagnostic("DynModel skipped empty vertex data block " +
                    std::to_string(element.vertexDataIndex));
                continue;
            }

            if (!orderGroupIndices.contains(element.vdOrderIndex))
            {
                orderGroupIndices.emplace(element.vdOrderIndex, orderedGroups.size());
                orderedGroups.push_back({});
            }
            orderedGroups[orderGroupIndices[element.vdOrderIndex]].push_back(&element);
        }

        for (const auto& group : orderedGroups)
        {
            for (const ShaderMeshElem* element : group)
            {
                if (element == nullptr || element->vertexDataIndex >= vertexData.size())
                {
                    continue;
                }

                const auto& source = vertexData[element->vertexDataIndex];
                const size_t sourceIndexCount = static_cast<size_t>(element->faceCount) * 3;
                if (static_cast<size_t>(element->startIndex) + sourceIndexCount > source.indices.size())
                {
                    AddDiagnostic("DynModel skipped primitive with out-of-range index span: startIndex=" +
                        std::to_string(element->startIndex) + ", indexCount=" +
                        std::to_string(sourceIndexCount) + ", sourceIndices=" +
                        std::to_string(source.indices.size()));
                    continue;
                }

                bool indicesInRange = true;
                for (size_t localIndex = 0; localIndex < sourceIndexCount; ++localIndex)
                {
                    const uint64_t localVertex =
                        static_cast<uint64_t>(source.indices[element->startIndex + localIndex]) +
                        static_cast<uint64_t>(element->baseVertex);
                    if (localVertex >= source.vertices.size())
                    {
                        indicesInRange = false;
                        break;
                    }
                }
                if (!indicesInRange)
                {
                    AddDiagnostic("DynModel skipped primitive with vertex index outside its vertex buffer.");
                    continue;
                }

                RendInstPrimitive primitive;
                primitive.materialIndex = element->materialIndex + static_cast<uint32_t>(materialIndexOffset);
                primitive.firstIndex = static_cast<uint32_t>(mesh.indices.size());
                primitive.indexCount = static_cast<uint32_t>(sourceIndexCount);
                mesh.primitives.push_back(primitive);

                for (size_t localIndex = 0; localIndex < sourceIndexCount; ++localIndex)
                {
                    const uint32_t localVertex =
                        source.indices[element->startIndex + localIndex] +
                        element->baseVertex;
                    const uint64_t vertexKey =
                        (static_cast<uint64_t>(element->vertexDataIndex) << 32) |
                        static_cast<uint64_t>(localVertex);

                    const auto existing = remappedVertices.find(vertexKey);
                    if (existing != remappedVertices.end())
                    {
                        mesh.indices.push_back(existing->second);
                        continue;
                    }

                    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max())
                    {
                        throw std::runtime_error("DynModel compacted mesh has too many vertices");
                    }

                    SkinnedVertex vertex = source.vertices[localVertex];
                    vertex.x = (vertex.x * positionScale[0]) + positionOffset[0];
                    vertex.y = (vertex.y * positionScale[1]) + positionOffset[1];
                    vertex.z = (vertex.z * positionScale[2]) + positionOffset[2];

                    const uint32_t remappedIndex = static_cast<uint32_t>(mesh.vertices.size());
                    mesh.vertices.push_back(vertex);
                    remappedVertices.emplace(vertexKey, remappedIndex);
                    mesh.indices.push_back(remappedIndex);
                }
            }
        }

        if (mesh.primitives.empty() && !mesh.indices.empty())
        {
            mesh.primitives.push_back({ 0, 0, static_cast<uint32_t>(mesh.indices.size()) });
        }

        computeNormals(mesh);
        return mesh;
    };

    m_lods.clear();
    m_decompressedPayload.clear();
    m_containerHeaderSize = 0;
    m_internalMaterials.clear();
    size_t numRigidMaterials = 0;

    DagorIO::MemoryReader rawReader(m_data);
    const DynModelPhase1Header header = ReadDynModelPhase1Header(rawReader);
    m_usesExternalMaterialTextureCounts = header.externalMaterialTextureCounts;
    if (!header.externalMaterialTextureCounts)
    {
        m_internalTextures = ReadDynModelTextureNames(rawReader, header.texCnt);
    }
    else
    {
        m_internalTextures = m_externalTextures;
    }

    const size_t mainMvdOffset = rawReader.GetPosition();
    std::vector<DecodedVertexData> mainVertexData;
    if (header.vdataNum > 0)
    {
        const CompressedBlock mainMvd = decompressBlock(m_data.subspan(mainMvdOffset));
        m_containerHeaderSize = mainMvdOffset;
        m_decompressedPayload = mainMvd.decompressed;
        rawReader.Seek(mainMvdOffset + mainMvd.rawSize);
        mainVertexData = decodeMatVData(m_decompressedPayload, MVD_NORMAL);
        m_internalMaterials = ParseInternalMaterials(m_decompressedPayload, m_internalTextures);
        numRigidMaterials = m_internalMaterials.size();
    }
    else
    {
        rawReader.Skip(0x20); // Skip padding if no MVD
    }

    LogDynModelPhase1Header(header, m_internalTextures, m_decompressedPayload.size(), m_containerHeaderSize);
    {
        std::ostringstream diag;
        diag << "Phase1 header: payload=" << m_data.size()
             << ", decompressedMvd=" << m_decompressedPayload.size()
             << ", containerHeader=" << m_containerHeaderSize
             << ", lodHdrInfo=0x" << std::hex << header.lodHdrInfo << std::dec
             << ", lodHdrSize=" << header.lodHdrSize
             << ", riStructSize=" << header.riStructSize
             << ", texCnt=" << (header.externalMaterialTextureCounts ? std::string("external") : std::to_string(header.texCnt))
             << ", matCnt=" << (header.externalMaterialTextureCounts ? std::string("external") : std::to_string(header.matCnt))
             << ", vdataNum=" << header.vdataNum
             << ", mvdHdrSize=" << header.mvdHdrSize
             << ", mvdHdrFlag=" << (header.mvdHdrFlag ? "true" : "false");
        AddDiagnostic(diag.str());
    }

    float positionOffset[3] = { 0.0f, 0.0f, 0.0f };
    float positionScale[3] = { 1.0f, 1.0f, 1.0f };
    std::vector<std::string> sceneNodeNames;
    std::unordered_map<uint32_t, size_t> skinNodeMap;
    std::vector<LodInfo> lods;
    std::vector<SkinnedLod> skinnedLods;
    std::vector<DecodedVertexData> skinnedVertexData;

    if (rawReader.GetRemaining() >= 16)
    {
        const size_t modelDataOffset = rawReader.GetPosition();
        const uint32_t modelDataSize = rawReader.ReadUInt32();
        const uint32_t lodCount = rawReader.ReadUInt32();

        if (lodCount > 100)
        {
            throw std::runtime_error("DynModel lodCount is unreasonable: " + std::to_string(lodCount));
        }
        if (!IsRangeWithin(modelDataOffset, static_cast<size_t>(modelDataSize), rawReader.GetSize()))
        {
            throw std::runtime_error("DynModel ModelData block exceeds payload bounds");
        }
        if (modelDataSize < 0x28)
        {
            throw std::runtime_error("DynModel ModelData block is truncated");
        }

        {
            std::ostringstream diag;
            diag << "DynModel ModelData: offset=" << modelDataOffset
                 << ", size=" << modelDataSize
                 << ", lodCount=" << lodCount;
            AddDiagnostic(diag.str());
        }

        rawReader.Skip(8);
        rawReader.Skip(24);

        if (modelDataSize > 0x28)
        {
            if (modelDataSize < 0x48)
            {
                throw std::runtime_error("DynModel ModelData scale/offset block is truncated");
            }
            positionOffset[0] = rawReader.ReadFloat();
            positionOffset[1] = rawReader.ReadFloat();
            positionOffset[2] = rawReader.ReadFloat();
            rawReader.ReadFloat();

            positionScale[0] = rawReader.ReadFloat();
            positionScale[1] = rawReader.ReadFloat();
            positionScale[2] = rawReader.ReadFloat();
            rawReader.ReadFloat();
        }

        // Force alignment to end of ModelData block (start of occlusion data)
        rawReader.Seek(modelDataOffset + modelDataSize);

        std::vector<uint32_t> shaderMeshOffsets;
        shaderMeshOffsets.reserve(lodCount);
        size_t occlusionBytes = 0;
        if (!CheckedMul(static_cast<size_t>(lodCount), static_cast<size_t>(16), occlusionBytes) ||
            occlusionBytes > rawReader.GetRemaining())
        {
            throw std::runtime_error("DynModel LOD occlusion table is truncated");
        }
        for (uint32_t lodId = 0; lodId < lodCount; ++lodId)
        {
            shaderMeshOffsets.push_back(rawReader.ReadUInt32());
            rawReader.Skip(4); // padding
            rawReader.Skip(4); // lodDistance
            rawReader.Skip(4); // padding
        }

        const size_t sceneBlockSizeOffset = rawReader.GetPosition();
        const uint32_t sceneBlockSize = rawReader.ReadUInt32();
        if (sceneBlockSize > rawReader.GetRemaining())
        {
            throw std::runtime_error("DynModel scene-node block exceeds payload bounds");
        }

        DagorIO::MemoryReader sceneReader(rawReader.ReadSpan(sceneBlockSize));
        if (sceneReader.GetRemaining() < 0x20)
        {
            throw std::runtime_error("DynModel scene-node block is truncated");
        }

        const uint32_t nameIndicesOfs = sceneReader.ReadUInt32();
        const uint32_t nameCount = sceneReader.ReadUInt32();

        if (nameCount > kMaxReasonableCount)
        {
            throw std::runtime_error("DynModel nameCount is unreasonable: " + std::to_string(nameCount));
        }

        sceneReader.Skip(8);
        const uint32_t skinNodesOfs = sceneReader.ReadUInt32();
        const uint32_t skinNodeCount = sceneReader.ReadUInt32();

        if (skinNodeCount > kMaxReasonableCount)
        {
            throw std::runtime_error("DynModel skinNodeCount is unreasonable: " + std::to_string(skinNodeCount));
        }

        sceneReader.Skip(8); // Header padding

        {
            std::ostringstream diag;
            diag << "DynModel scene nodes: blockOffset=" << sceneBlockSizeOffset
                 << ", blockSize=" << sceneBlockSize
                 << ", nameIndicesOfs=" << nameIndicesOfs
                 << ", nameCount=" << nameCount
                 << ", skinNodesOfs=" << skinNodesOfs
                 << ", skinNodeCount=" << skinNodeCount;
            AddDiagnostic(diag.str());
        }

        if (nameIndicesOfs < 0x20)
        {
            throw std::runtime_error("DynModel scene-node name table is malformed");
        }

        size_t nameIndexBytes = 0;
        if (!CheckedMul(static_cast<size_t>(nameCount), sizeof(uint64_t), nameIndexBytes) ||
            !IsRangeWithin(static_cast<size_t>(nameIndicesOfs), nameIndexBytes, sceneReader.GetSize()))
        {
            throw std::runtime_error("DynModel scene-node name index table is out of bounds");
        }

        size_t skinNodeBytes = 0;
        if (!CheckedMul(static_cast<size_t>(skinNodeCount), sizeof(uint16_t), skinNodeBytes) ||
            !IsRangeWithin(static_cast<size_t>(skinNodesOfs), skinNodeBytes, sceneReader.GetSize()))
        {
            throw std::runtime_error("DynModel scene-node skin table is out of bounds");
        }

        const size_t nameBlobSize = nameIndicesOfs - 0x20;
        if (sceneReader.GetRemaining() < nameBlobSize)
        {
             throw std::runtime_error("DynModel name blob truncated");
        }

        const std::span<const uint8_t> nameBlob = sceneReader.ReadSpan(nameBlobSize);
        std::vector<uint64_t> nameOffsets;
        nameOffsets.reserve(nameCount);
        for (uint32_t nameIndex = 0; nameIndex < nameCount; ++nameIndex)
        {
            nameOffsets.push_back(sceneReader.ReadUInt64());
        }

        sceneNodeNames.reserve(nameCount);
        for (uint32_t nameIndex = 0; nameIndex < nameCount; ++nameIndex)
        {
            const size_t begin = (nameOffsets[nameIndex] >= 0x20) ? static_cast<size_t>(nameOffsets[nameIndex] - 0x20) : 0;
            const size_t end = (nameIndex + 1 < nameCount)
                ? (nameOffsets[nameIndex + 1] >= 0x20 ? static_cast<size_t>(nameOffsets[nameIndex + 1] - 0x20) : nameBlob.size())
                : nameBlob.size();

            if (begin >= nameBlob.size() || end > nameBlob.size() || begin >= end)
            {
                sceneNodeNames.push_back("");
                continue;
            }
            sceneNodeNames.push_back(TrimDagorString(nameBlob.subspan(begin, end - begin)));
        }

        sceneReader.Seek(skinNodesOfs);
        for (uint32_t i = 0; i < skinNodeCount; ++i)
        {
            skinNodeMap.emplace(sceneReader.ReadUInt16(), i);
        }

        lods.reserve(lodCount);
        for (uint32_t lodId = 0; lodId < lodCount; ++lodId)
        {
            const size_t lodStart = rawReader.GetPosition();
            LodInfo lod;
            lod.lodId = lodId;
            const uint32_t lodHeaderSize = rawReader.ReadUInt32();
            const uint32_t rigidCount = rawReader.ReadUInt32();

            if (rigidCount > kMaxReasonableCount)
            {
                throw std::runtime_error("DynModel rigidCount is unreasonable: " + std::to_string(rigidCount));
            }
            if (lodHeaderSize < 0x20)
            {
                throw std::runtime_error("DynModel LOD header is malformed");
            }

            rawReader.Skip(8);
            rawReader.ReadUInt32(); // skinPtr
            lod.skinCount = rawReader.ReadUInt32();

            if (lod.skinCount > kMaxReasonableCount)
            {
                throw std::runtime_error("DynModel skinCount is unreasonable: " + std::to_string(lod.skinCount));
            }

            rawReader.Skip(8);

            {
                std::ostringstream diag;
                diag << "DynModel LOD[" << lodId << "]: start=" << lodStart
                     << ", headerSize=" << lodHeaderSize
                     << ", rigidCount=" << rigidCount
                     << ", skinCount=" << lod.skinCount
                     << ", shaderMeshOfs=" << shaderMeshOffsets[lodId];
                AddDiagnostic(diag.str());
            }

            size_t rigidBytes = 0;
            if (!CheckedMul(static_cast<size_t>(rigidCount), static_cast<size_t>(0x20), rigidBytes) ||
                rigidBytes > rawReader.GetRemaining())
            {
                throw std::runtime_error("DynModel rigid object table is out of bounds");
            }

            lod.rigidObjects.reserve(rigidCount);
            for (uint32_t rigidIndex = 0; rigidIndex < rigidCount; ++rigidIndex)
            {
                RigidObject rigid;
                rawReader.ReadUInt64(); // shaderMeshPtr
                rawReader.Skip(12); // sph_c
                rawReader.Skip(4); // sph_r
                rigid.nodeId = rawReader.ReadUInt32();
                rawReader.ReadUInt32(); // _resv
                lod.rigidObjects.push_back(rigid);
            }

            const size_t minShaderMeshOffset = 0x20 + rigidBytes;
            if (shaderMeshOffsets[lodId] < minShaderMeshOffset)
            {
                throw std::runtime_error("DynModel ShaderMesh offset overlaps the LOD header/rigid table");
            }

            size_t shaderMeshPos = 0;
            if (!CheckedAdd(lodStart, static_cast<size_t>(shaderMeshOffsets[lodId]), shaderMeshPos) ||
                shaderMeshPos > rawReader.GetSize())
            {
                throw std::runtime_error("DynModel ShaderMesh offset is out of bounds");
            }

            rawReader.Seek(shaderMeshPos);
            lod.rigidMeshes.reserve(rigidCount);
            for (uint32_t rigidIndex = 0; rigidIndex < rigidCount; ++rigidIndex)
            {
                lod.rigidMeshes.push_back(readShaderMesh(rawReader));
            }

            {
                std::ostringstream diag;
                diag << "DynModel LOD[" << lodId << "] ShaderMeshes end at " << rawReader.GetPosition();
                AddDiagnostic(diag.str());
            }
            lods.push_back(std::move(lod));
        }

        skinnedLods.resize(lodCount);
        if (rawReader.GetRemaining() >= 8)
        {
            const uint32_t skinnedResourceCount = rawReader.ReadUInt32();
            AddDiagnostic("DynModel skinned resource count=" + std::to_string(skinnedResourceCount));
            if (skinnedResourceCount > 1)
            {
                AddDiagnostic("Skipped optional skinned mesh resources: multiple resources are not supported yet.");
            }
            else if (skinnedResourceCount > 0)
            {
                try
                {
                    const uint32_t skinnedResourceSize = rawReader.ReadUInt32();
                    if (skinnedResourceSize > rawReader.GetRemaining())
                    {
                        throw std::runtime_error("DynModel skinned mesh resource exceeds payload bounds");
                    }

                    AddDiagnostic("DynModel skinned resource size=" + std::to_string(skinnedResourceSize));
                    DagorIO::MemoryReader skinnedReader(rawReader.ReadSpan(skinnedResourceSize));
                    if (skinnedReader.GetRemaining() < 0x18)
                    {
                        throw std::runtime_error("DynModel skinned mesh resource header is truncated");
                    }
                    skinnedReader.ReadUInt64();
                    skinnedReader.ReadUInt32();
                    skinnedReader.ReadUInt32();
                    skinnedReader.ReadUInt32();
                    skinnedReader.ReadUInt32();

                    const size_t skinnedMvdOffset = skinnedReader.GetPosition();
                    const CompressedBlock skinnedMvd = decompressBlock(
                        skinnedReader.ReadSpan(skinnedReader.GetRemaining()));
                    skinnedReader.Seek(skinnedMvdOffset + skinnedMvd.rawSize);
                    skinnedVertexData = decodeMatVData(
                        skinnedMvd.decompressed,
                        header.mvdHdrFlag ? MVD_SKINNED : MVD_SKINNED_FLAG);
                    auto skinnedMaterials = ParseInternalMaterials(skinnedMvd.decompressed, m_internalTextures);
                    m_internalMaterials.insert(m_internalMaterials.end(), skinnedMaterials.begin(), skinnedMaterials.end());

                    const std::span<const uint8_t> skinsCompressed =
                        skinnedReader.ReadSpan(skinnedReader.GetRemaining());
                    {
                        std::ostringstream diag;
                        diag << "DynModel skinned skin-tail: offset=" << skinnedReader.GetPosition()
                             << ", compressedBytes=" << skinsCompressed.size()
                             << ", firstBytes=" << HexPrefix(skinsCompressed, 12);
                        AddDiagnostic(diag.str());
                    }
                    if (!LooksLikeZlibHeader(skinsCompressed))
                    {
                        throw std::runtime_error("DynModel skinned skin-tail is not a recognized zlib stream");
                    }

                    const std::vector<uint8_t> skinsData =
                        DagorIO::Decompressor::DecompressZlib(skinsCompressed);
                    DagorIO::MemoryReader skinsReader(skinsData);

                    for (size_t lodIndex = 0; lodIndex < lods.size(); ++lodIndex)
                    {
                        auto& skinnedLod = skinnedLods[lodIndex];
                        const uint32_t skinCount = lods[lodIndex].skinCount;
                        skinnedLod.skinNodes.reserve(skinCount);
                        for (uint32_t skinIdx = 0; skinIdx < skinCount; ++skinIdx)
                        {
                            skinnedLod.skinNodes.push_back(skinsReader.ReadUInt32());
                        }

                        std::vector<uint64_t> blockSizes;
                        blockSizes.reserve(skinCount);
                        for (uint32_t skinIdx = 0; skinIdx < skinCount; ++skinIdx)
                        {
                            const uint64_t rawBlockSize = skinsReader.ReadUInt64();
                            const uint64_t blockSize = rawBlockSize & kBlockSizeMask;
                            if (blockSize > skinsReader.GetRemaining())
                            {
                                throw std::runtime_error("DynModel skinned mesh block exceeds the skin payload bounds");
                            }
                            blockSizes.push_back(blockSize);
                        }

                        skinnedLod.meshes.reserve(skinCount);
                        for (uint32_t skinIdx = 0; skinIdx < skinCount; ++skinIdx)
                        {
                            DagorIO::MemoryReader blockReader(skinsReader.ReadSpan(static_cast<size_t>(blockSizes[skinIdx])));
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.Skip(8);
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.Skip(8);
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            blockReader.ReadUInt32();
                            skinnedLod.meshes.push_back(readShaderMesh(blockReader));
                        }
                    }
                }
                catch (const std::exception& ex)
                {
                    skinnedVertexData.clear();
                    skinnedLods.assign(lodCount, {});
                    AddDiagnostic(std::string("Skipped optional skinned mesh resource: ") + ex.what());
                }
            }
        }
    }

    const uint32_t exportLod = 0;
    std::vector<uint32_t> rawMaterialIds;
    auto appendMaterialIds = [&](const std::vector<ShaderMesh>& meshes)
    {
        for (const ShaderMesh& mesh : meshes)
        {
            for (const ShaderMeshElem& element : mesh.elements)
            {
                rawMaterialIds.push_back(element.materialIndex);
            }
        }
    };

    if (exportLod < lods.size())
    {
        appendMaterialIds(lods[exportLod].rigidMeshes);
    }
    if (exportLod < skinnedLods.size())
    {
        appendMaterialIds(skinnedLods[exportLod].meshes);
    }

    std::sort(rawMaterialIds.begin(), rawMaterialIds.end());
    rawMaterialIds.erase(std::unique(rawMaterialIds.begin(), rawMaterialIds.end()), rawMaterialIds.end());
    if (!rawMaterialIds.empty())
    {
        const uint32_t maxRawMaterialSlot = rawMaterialIds.back();
        std::ostringstream diag;
        diag << "DynModel material index summary: uniqueRaw=" << rawMaterialIds.size()
             << ", maxRaw=" << maxRawMaterialSlot
             << ", mode=raw";
        AddDiagnostic(diag.str());
    }

    if (exportLod < lods.size())
    {
        const auto& lod = lods[exportLod];
        for (size_t rigidIndex = 0; rigidIndex < lod.rigidMeshes.size(); ++rigidIndex)
        {
            const std::string parentNodeName = rigidIndex < lod.rigidObjects.size()
                ? resolveNodeName(sceneNodeNames, skinNodeMap, lod.rigidObjects[rigidIndex].nodeId)
                : std::string();
            const std::string meshName = !parentNodeName.empty()
                ? parentNodeName
                : ("lod" + std::to_string(exportLod) + "_rigid" + std::to_string(rigidIndex));
            DynModelMesh mesh = assembleMesh(
                meshName,
                parentNodeName,
                lod.rigidMeshes[rigidIndex].elements,
                mainVertexData,
                positionScale,
                positionOffset,
                false,
                0);
            if (!mesh.vertices.empty() && !mesh.indices.empty())
            {
                m_lods.push_back(std::move(mesh));
            }
        }
    }

    if (exportLod < skinnedLods.size())
    {
        const auto& skinnedLod = skinnedLods[exportLod];
        for (size_t skinIndex = 0; skinIndex < skinnedLod.meshes.size(); ++skinIndex)
        {
            const std::string parentNodeName = skinIndex < skinnedLod.skinNodes.size()
                ? resolveNodeName(sceneNodeNames, skinNodeMap, skinnedLod.skinNodes[skinIndex])
                : std::string();
            const std::string meshName = !parentNodeName.empty()
                ? parentNodeName
                : ("lod" + std::to_string(exportLod) + "_skin" + std::to_string(skinIndex));
            DynModelMesh mesh = assembleMesh(
                meshName,
                parentNodeName,
                skinnedLod.meshes[skinIndex].elements,
                skinnedVertexData,
                positionScale,
                positionOffset,
                true,
                numRigidMaterials);
            if (!mesh.vertices.empty() && !mesh.indices.empty())
            {
                m_lods.push_back(std::move(mesh));
            }
        }
    }

    if (m_lods.empty())
    {
        for (const auto& block : mainVertexData)
        {
            DynModelMesh mesh;
            mesh.name = "lod" + std::to_string(block.lodId);
            mesh.vertices = block.vertices;
            for (auto& vertex : mesh.vertices)
            {
                vertex.x = (vertex.x * positionScale[0]) + positionOffset[0];
                vertex.y = (vertex.y * positionScale[1]) + positionOffset[1];
                vertex.z = (vertex.z * positionScale[2]) + positionOffset[2];
            }
            mesh.indices = block.indices;
            if (!mesh.indices.empty())
            {
                mesh.primitives.push_back({ 0, 0, static_cast<uint32_t>(mesh.indices.size()) });
            }
            computeNormals(mesh);
            if (!mesh.vertices.empty() && !mesh.indices.empty())
            {
                m_lods.push_back(std::move(mesh));
            }
        }
    }
}

} // namespace DagorFormats
