#include "DynModelReader.h"
#include "RendInstReader.h"
#include "DagorIO/Decompression.h"
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>

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

        const size_t stringBlobStart = reader.GetPosition();
        const uint64_t indexTablePos64 =
            static_cast<uint64_t>(texIndicesOfs) + kTextureIndexOffsetBias;
        if (indexTablePos64 > reader.GetSize())
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

} // namespace

DynModelReader::DynModelReader(std::span<const uint8_t> data) : m_data(data) {}

void DynModelReader::Parse() {
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
    std::vector<uint8_t> decompressedData;
    bool foundCompressedBlock = false;
    const uint8_t zstdMagic[] = { 0x28, 0xB5, 0x2F, 0xFD };
    
    for (size_t scanPos = 4; scanPos + 4 <= m_data.size(); ++scanPos)
    {
        if (std::memcmp(m_data.data() + scanPos, zstdMagic, 4) == 0)
        {
            try
            {
                decompressedData = DagorIO::Decompressor::DecompressZstd(m_data.subspan(scanPos));
                foundCompressedBlock = true;
                m_containerHeaderSize = scanPos;
                break;
            }
            catch (...) { continue; }
        }
    }

    if (!foundCompressedBlock)
    {
        for (size_t headerOfs = 0; headerOfs + 4 <= m_data.size() && headerOfs < 256; headerOfs += 4)
        {
            const uint32_t blockHeader = ReadU32(m_data, headerOfs);
            const unsigned blockFlags = blockHeader >> 30;
            const size_t blockSize = blockHeader & kBlockSizeMask;

            if (blockFlags == kBlockCompressionZstd && blockSize > 8 && blockSize + headerOfs + 4 <= m_data.size())
            {
                try
                {
                    decompressedData = DagorIO::Decompressor::DecompressZstd(m_data.subspan(headerOfs + 4, blockSize));
                    foundCompressedBlock = true;
                    m_containerHeaderSize = headerOfs;
                    break;
                }
                catch (...) { continue; }
            }
            else if (blockFlags == kBlockCompressionOodle && blockSize > 8 && blockSize + headerOfs + 4 <= m_data.size())
            {
                try
                {
                    auto blockData = m_data.subspan(headerOfs + 4, blockSize);
                    if (blockData.size() >= sizeof(uint32_t))
                    {
                        const uint32_t decompSz = ReadU32(blockData, 0);
                        decompressedData = DagorIO::Decompressor::DecompressOodle(
                            blockData.subspan(sizeof(uint32_t)), decompSz, "", "");
                        foundCompressedBlock = true;
                        m_containerHeaderSize = headerOfs;
                        break;
                    }
                }
                catch (...) { continue; }
            }
        }
    }

    if (!foundCompressedBlock)
    {
        decompressedData.assign(m_data.begin(), m_data.end());
        m_containerHeaderSize = 0;
    }

    m_decompressedPayload = std::move(decompressedData);
    DagorIO::MemoryReader rawReader(m_data);
    const DynModelPhase1Header header = ReadDynModelPhase1Header(rawReader);
    std::vector<std::string> textureNames;
    if (!header.externalMaterialTextureCounts) textureNames = ReadDynModelTextureNames(rawReader, header.texCnt);

    LogDynModelPhase1Header(header, textureNames, m_decompressedPayload.size(), m_containerHeaderSize);

    const size_t mvdStartPos = foundCompressedBlock ? 0 : rawReader.GetPosition();
    DagorIO::MemoryReader meshReader(m_decompressedPayload);
    meshReader.Seek(mvdStartPos);

    if (meshReader.GetRemaining() < 24) throw std::runtime_error("DynModel MatVData header is truncated");

    const uint32_t matOfs = meshReader.ReadUInt32();
    const uint32_t matCnt = meshReader.ReadUInt32();
    meshReader.Skip(8);
    const uint32_t mvdGvdOfs = meshReader.ReadUInt32();
    const uint32_t mvdGvdCnt = meshReader.ReadUInt32();

    if (mvdGvdOfs > 0) meshReader.Seek(mvdStartPos + mvdGvdOfs);

    const int mvdFlag = header.mvdHdrFlag ? MVD_SKINNED : MVD_SKINNED_FLAG;
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
        gvd.storageFormat = meshReader.ReadUInt32();
        gvd.storageFormat += mvdFlag * 10;
        meshReader.Skip(8);

        std::cout << "GVD Block " << i << ": vCnt=" << gvd.vCnt << ", vStride=" << (uint32_t)gvd.vStride << ", storageFormat=" << gvd.storageFormat << std::endl;
        gvDataList.push_back(gvd);
    }

    const size_t vdStartOfs = mvdStartPos + mvdGvdOfs + (static_cast<size_t>(mvdGvdCnt) * 0x20);
    meshReader.Seek(vdStartOfs);

    size_t totalVerticesDecoded = 0;
    size_t totalIndicesDecoded = 0;

    for (uint32_t i = 0; i < mvdGvdCnt; ++i)
    {
        const auto& gvd = gvDataList[i];
        const size_t blockStartPos = meshReader.GetPosition();
        std::vector<SkinnedVertex> decodedVerts;
        decodedVerts.reserve(gvd.vCnt);

        for (uint32_t vIdx = 0; vIdx < gvd.vCnt; ++vIdx)
        {
            const size_t vertexStartPos = meshReader.GetPosition();
            SkinnedVertex v = {};
            // Set default joint indices and weights to bind to root bone (bone 0) with weight 1.0
            // so that if a skeleton is present, the model renders in its base pose without deformation.
            v.jointIndices[0] = 0; v.jointIndices[1] = 0; v.jointIndices[2] = 0; v.jointIndices[3] = 0;
            v.jointWeights[0] = 1.0f; v.jointWeights[1] = 0.0f; v.jointWeights[2] = 0.0f; v.jointWeights[3] = 0.0f;

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
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(2);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 16) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }
            else if (fmt == 3) {
                if (stride == 16) {
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 20) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 24) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = meshReader.ReadFloat();
                    v.v = -meshReader.ReadFloat();
                    decoded = true;
                }
            }
            else if (fmt == 4) {
                if (stride == 20) {
                    meshReader.Skip(4);
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 28) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = meshReader.ReadFloat();
                    v.v = -meshReader.ReadFloat();
                    decoded = true;
                }
            }
            else if (fmt == 5) {
                if (stride == 24) {
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    decoded = true;
                }
                else if (stride == 28) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(12);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 36) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = meshReader.ReadFloat();
                    v.v = -meshReader.ReadFloat();
                    decoded = true;
                }
            }
            else if (fmt == 6) {
                if (stride == 32) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }
            else if (fmt == 13) {
                if (stride == 16) {
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 20) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 24) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = meshReader.ReadFloat();
                    v.v = -meshReader.ReadFloat();
                    decoded = true;
                }
            }
            else if (fmt == 15) {
                if (stride == 24) {
                    meshReader.Skip(8);
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 28) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(8);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 32) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    v.u = 0.0f; v.v = 0.0f;
                    decoded = true;
                }
            }
            else if (fmt == 16) {
                if (stride == 28) {
                    meshReader.Skip(12);
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 32) {
                    meshReader.Skip(4);
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 36) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(14);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }
            else if (fmt == 17) {
                if (stride == 32) {
                    meshReader.Skip(8);
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    v.u = 0.0f; v.v = 0.0f;
                    decoded = true;
                }
                else if (stride == 36) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(8);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }
            else if (fmt == 25) {
                if (stride == 24) {
                    v.x = (float)meshReader.ReadInt16() / 32768.0f;
                    v.y = (float)meshReader.ReadInt16() / 32768.0f;
                    v.z = (float)meshReader.ReadInt16() / 32768.0f;
                    meshReader.Skip(6);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 28) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
                else if (stride == 32) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = meshReader.ReadFloat();
                    v.v = -meshReader.ReadFloat();
                    decoded = true;
                }
            }
            else if (fmt == 26) {
                if (stride == 32) {
                    meshReader.Skip(4);
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }
            else if (fmt == 27) {
                if (stride == 36) {
                    v.x = meshReader.ReadFloat(); v.y = meshReader.ReadFloat(); v.z = meshReader.ReadFloat();
                    meshReader.Skip(4);
                    v.u = (float)meshReader.ReadInt16() / 4096.0f;
                    v.v = 1.0f - ((float)meshReader.ReadInt16() / 4096.0f);
                    decoded = true;
                }
            }

            if (!decoded) throw std::runtime_error("Unimplemented vertex storage format: " + std::to_string(gvd.storageFormat) + " stride: " + std::to_string(gvd.vStride));
            decodedVerts.push_back(v);
            meshReader.Seek(vertexStartPos + gvd.vStride);
        }
        totalVerticesDecoded += decodedVerts.size();

        meshReader.Seek(blockStartPos + static_cast<size_t>(gvd.vCnt) * gvd.vStride);

        std::vector<uint32_t> decodedIndices;
        decodedIndices.reserve(gvd.iCnt);

        if (gvd.iPackedSz == 0)
        {
            size_t numIndices = gvd.iSz / 2;
            for (size_t i = 0; i < numIndices; i += 3)
            {
                if (i + 2 < numIndices)
                {
                    uint16_t i0 = meshReader.ReadUInt16();
                    uint16_t i1 = meshReader.ReadUInt16();
                    uint16_t i2 = meshReader.ReadUInt16();
                    decodedIndices.push_back(i0);
                    decodedIndices.push_back(i2);
                    decodedIndices.push_back(i1);
                }
                else
                {
                    for (size_t j = i; j < numIndices; ++j)
                        decodedIndices.push_back(meshReader.ReadUInt16());
                }
            }
        }
        else
        {
            const size_t vbSz = static_cast<size_t>(gvd.vCnt) * gvd.vStride;
            const size_t streamEnd = blockStartPos + vbSz + gvd.iPackedSz - 4;

            meshReader.Skip(1);
            int32_t stateBuffer[2] = {0, 0};
            std::vector<uint32_t> tempIndices;

            while (meshReader.GetPosition() < streamEnd)
            {
                uint32_t decodedLeb = 0;
                uint32_t shift = 0;
                while (true) {
                    if (meshReader.GetPosition() >= streamEnd) break;
                    uint8_t byte = meshReader.ReadUInt8();
                    decodedLeb |= (static_cast<uint32_t>(byte & 0x7F) << shift);
                    shift += 7;
                    if ((byte & 0x80) == 0) break;
                }
                uint32_t current = decodedLeb & 1;
                int32_t delta = (decodedLeb >> 2) ^ -(static_cast<int32_t>((decodedLeb & 2) != 0));
                int32_t index = stateBuffer[current] + delta;
                stateBuffer[current] = index;
                tempIndices.push_back(index);
            }

            for (size_t i = 0; i < tempIndices.size(); i += 3)
            {
                if (i + 2 < tempIndices.size())
                {
                    decodedIndices.push_back(tempIndices[i]);
                    decodedIndices.push_back(tempIndices[i + 2]);
                    decodedIndices.push_back(tempIndices[i + 1]);
                }
                else
                {
                    for (size_t j = i; j < tempIndices.size(); ++j)
                        decodedIndices.push_back(tempIndices[j]);
                }
            }
        }

        totalIndicesDecoded += decodedIndices.size();

        const size_t vbSz = static_cast<size_t>(gvd.vCnt) * gvd.vStride;
        const size_t ibSz = (gvd.iPackedSz == 0) ? gvd.iSz : gvd.iPackedSz;
        meshReader.Seek(blockStartPos + vbSz + ibSz);
        
        // --- PHASE 5: Mesh Assembly ---
        uint32_t lodId = (gvd.flags & 0xF000) >> 12;
        
        // Ensure m_lods has enough elements
        while (m_lods.size() <= lodId)
        {
            DynModelMesh newLod;
            newLod.name = "lod" + std::to_string(m_lods.size());
            m_lods.push_back(std::move(newLod));
        }
        
        DynModelMesh& lod = m_lods[lodId];
        
        // Add primitive for this GVD block
        RendInstPrimitive prim;
        prim.materialIndex = 0; // Proper material assignment requires ShaderMesh parsing
        prim.firstIndex = static_cast<uint32_t>(lod.indices.size());
        prim.indexCount = static_cast<uint32_t>(decodedIndices.size());
        lod.primitives.push_back(prim);
        
        // Append indices, offset by current vertex count
        uint32_t vertexOffset = static_cast<uint32_t>(lod.vertices.size());
        for (uint32_t idx : decodedIndices)
        {
            lod.indices.push_back(idx + vertexOffset);
        }
        
        // Append vertices
        lod.vertices.insert(lod.vertices.end(), decodedVerts.begin(), decodedVerts.end());
    }

    std::cout << "Phase 5: Successfully assembled " << m_lods.size() << " LODs from " << mvdGvdCnt << " GVD blocks." << std::endl;
}

} // namespace DagorFormats
