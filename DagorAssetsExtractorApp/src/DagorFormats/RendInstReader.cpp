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

    struct RendInstPhase1Header
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
            throw std::runtime_error("RendInst read out of bounds");
        }
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
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

    RendInstPhase1Header ReadRendInstPhase1Header(DagorIO::MemoryReader& reader)
    {
        if (reader.GetRemaining() < sizeof(uint32_t) * 5)
        {
            throw std::runtime_error("RendInst ShaderMeshResource header is truncated");
        }

        RendInstPhase1Header header;
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

    std::vector<std::string> ReadRendInstTextureNames(
        DagorIO::MemoryReader& reader,
        uint32_t headerTextureCount)
    {
        if (headerTextureCount == 0)
        {
            return {};
        }

        if (reader.GetRemaining() < kTextureMapHeaderSize)
        {
            throw std::runtime_error("RendInst texture map header is truncated");
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
            throw std::runtime_error("RendInst texture name index table points outside the payload");
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

} // namespace

RendInstReader::RendInstReader(std::span<const uint8_t> data) : m_data(data) {}

void RendInstReader::Parse() {
    if (m_data.size() >= 4 && std::memcmp(m_data.data(), "RIND", 4) == 0)
    {
        DagorIO::MemoryReader reader(m_data);

        if (reader.GetRemaining() < 8) {
            throw std::runtime_error("RendInst data too small");
        }

        // Check magic
        std::string magic = reader.ReadString(4);
        if (magic != "RIND") {
            throw std::runtime_error("Invalid RendInst magic: " + magic);
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
    else
    {
        ParseRealGrpPayload();
    }
}

void RendInstReader::ParseRealGrpPayload() {
    // Scan for zstd frame magic (0xFD2FB528)
    const uint8_t zstdMagic[] = { 0x28, 0xB5, 0x2F, 0xFD };
    bool foundCompressedBlock = false;
    
    for (size_t scanPos = 4; scanPos + 4 <= m_data.size(); ++scanPos)
    {
        if (std::memcmp(m_data.data() + scanPos, zstdMagic, 4) == 0)
        {
            try
            {
                m_decompressedPayload = DagorIO::Decompressor::DecompressZstd(m_data.subspan(scanPos));
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
            const size_t blockSize = blockHeader & 0x3FFFFFFF;

            if (blockFlags == 1 && blockSize > 8 && blockSize + headerOfs + 4 <= m_data.size())
            {
                try
                {
                    m_decompressedPayload = DagorIO::Decompressor::DecompressZstd(m_data.subspan(headerOfs + 4, blockSize));
                    foundCompressedBlock = true;
                    m_containerHeaderSize = headerOfs;
                    break;
                }
                catch (...) { continue; }
            }
            else if (blockFlags == 2 && blockSize > 8 && blockSize + headerOfs + 4 <= m_data.size())
            {
                try
                {
                    auto blockData = m_data.subspan(headerOfs + 4, blockSize);
                    if (blockData.size() >= sizeof(uint32_t))
                    {
                        const uint32_t decompSz = ReadU32(blockData, 0);
                        m_decompressedPayload = DagorIO::Decompressor::DecompressOodle(
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
        m_decompressedPayload.assign(m_data.begin(), m_data.end());
        m_containerHeaderSize = 0;
    }

    DagorIO::MemoryReader rawReader(m_data);
    const RendInstPhase1Header header = ReadRendInstPhase1Header(rawReader);
    std::cout << "[DEBUG RendInst] lodHdrInfo=" << header.lodHdrInfo
              << ", lodHdrSize=" << header.lodHdrSize
              << ", riStructSize=" << header.riStructSize
              << ", texCnt=" << header.texCnt
              << ", matCnt=" << header.matCnt
              << ", vdataNum=" << header.vdataNum
              << ", mvdHdrPacked=" << header.mvdHdrPacked
              << ", mvdHdrSize=" << header.mvdHdrSize
              << ", extMatTex=" << (header.externalMaterialTextureCounts ? 1 : 0)
              << ", decompressedPayloadSize=" << m_decompressedPayload.size()
              << std::endl;

    std::cout << "[DEBUG RendInst] Hex Dump (64 bytes): ";
    for (size_t i = 0; i < std::min<size_t>(64, m_decompressedPayload.size()); ++i)
    {
        printf("%02X ", m_decompressedPayload[i]);
    }
    std::cout << std::endl;

    std::vector<std::string> textureNames;
    if (!header.externalMaterialTextureCounts)
    {
        textureNames = ReadRendInstTextureNames(rawReader, header.texCnt);
    }

    // Phase 2: MatVData parsing
    const size_t mvdStartPos = foundCompressedBlock ? 0 : rawReader.GetPosition();
    DagorIO::MemoryReader meshReader(m_decompressedPayload);
    meshReader.Seek(mvdStartPos);
    std::cout << "[DEBUG RendInst] mvdStartPos=" << mvdStartPos
              << ", remaining=" << meshReader.GetRemaining()
              << std::endl;

    if (meshReader.GetRemaining() < 24)
    {
        throw std::runtime_error("RendInst MatVData header is truncated");
    }

    const uint32_t matOfs = meshReader.ReadUInt32();
    const uint32_t matCnt = meshReader.ReadUInt32();
    meshReader.Skip(8);
    const uint32_t mvdGvdOfs = meshReader.ReadUInt32();
    const uint32_t mvdGvdCnt = meshReader.ReadUInt32();

    std::cout << "[DEBUG RendInst] matOfs=" << matOfs
              << ", matCnt=" << matCnt
              << ", mvdGvdOfs=" << mvdGvdOfs
              << ", mvdGvdCnt=" << mvdGvdCnt
              << std::endl;

    if (mvdGvdOfs > 0)
    {
        meshReader.Seek(mvdStartPos + mvdGvdOfs);
    }

    const int mvdFlag = MVD_NORMAL; // RendInst usually uses normal vertex data
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

        std::cout << "RendInst GVD Block " << i << ": vCnt=" << gvd.vCnt 
                  << ", vStride=" << static_cast<uint32_t>(gvd.vStride) 
                  << ", storageFormat=" << gvd.storageFormat << std::endl;
        gvDataList.push_back(gvd);
    }

    // Phase 3: Vertex Stream Decoding
    const size_t vdStartOfs = mvdStartPos + mvdGvdOfs + (static_cast<size_t>(mvdGvdCnt) * 0x20);
    meshReader.Seek(vdStartOfs);

    size_t totalVerticesDecoded = 0;
    size_t totalIndicesDecoded = 0;

    for (uint32_t i = 0; i < mvdGvdCnt; ++i)
    {
        const auto& gvd = gvDataList[i];
        const size_t blockStartPos = meshReader.GetPosition();
        std::vector<Vertex> decodedVerts;
        decodedVerts.reserve(gvd.vCnt);

        for (uint32_t vIdx = 0; vIdx < gvd.vCnt; ++vIdx)
        {
            const size_t vertexStartPos = meshReader.GetPosition();
            Vertex v = {};
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
            RendInstMesh newLod;
            newLod.name = "lod" + std::to_string(m_lods.size());
            m_lods.push_back(std::move(newLod));
        }
        
        RendInstMesh& lod = m_lods[lodId];
        
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
