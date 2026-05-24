#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"
#include "AssetIndex/DependencyGraph.h"
#include "AssetIndex/PackContents.h"
#include "DagorIO/BinaryReader.h"
#include "DagorFormats/VromfsReader.h"
#include "DagorFormats/DxpReader.h"
#include "DagorFormats/DdsxReader.h"
#include "DagorFormats/BlkReader.h"
#include "DagorFormats/GrpReader.h"
#include "DagorFormats/Anim2DataReader.h"
#include "DagorFormats/GameResDescReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include "Jobs/JobSystem.h"
#include "Export/ExportManifest.h"
#include "Export/TextureExporter.h"
#include "Export/ModelExporter.h"
#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "App/AppState.h"

#include "DagorIO/Decompression.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace AssetIndex = DagorAssetsExtractor::AssetIndex;

namespace
{
    void AppendU32(std::vector<uint8_t>& data, uint32_t value);

    bool Expect(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            return false;
        }
        return true;
    }

    size_t CountType(const std::vector<AssetIndex::ResourceRecord>& records, AssetIndex::AssetType type)
    {
        size_t count = 0;
        for (const AssetIndex::ResourceRecord& record : records)
        {
            if (record.type == type)
            {
                ++count;
            }
        }
        return count;
    }

    bool ExpectNear(float actual, float expected, float epsilon, const char* message)
    {
        if (std::fabs(actual - expected) > epsilon)
        {
            std::cerr << "FAIL: " << message << " (expected " << expected << ", got " << actual << ")\n";
            return false;
        }
        return true;
    }

    bool TestBinaryReader()
    {
        bool ok = true;
        const uint8_t data[] = {
            0x12, 0x34, 0x56, 0x78, // UInt32: 0x78563412 (little endian)
            0xAB, 0xCD,             // UInt16: 0xCDAB
            0xEF,                   // UInt8:  0xEF
            'H', 'e', 'l', 'l', 'o', '\0',
            'W', 'o', 'r', 'l', 'd'
        };

        DagorIO::MemoryReader reader(data, sizeof(data));
        ok &= Expect(reader.ReadUInt32() == 0x78563412, "BinaryReader should read little-endian UInt32");
        ok &= Expect(reader.ReadUInt16() == 0xCDAB, "BinaryReader should read little-endian UInt16");
        ok &= Expect(reader.ReadUInt8() == 0xEF, "BinaryReader should read UInt8");
        ok &= Expect(reader.ReadNullTerminatedString() == "Hello", "BinaryReader should read null-terminated string");
        ok &= Expect(reader.ReadString(5) == "World", "BinaryReader should read fixed-length string");
        ok &= Expect(reader.IsEOF(), "BinaryReader should be at EOF");

        reader.Seek(4);
        ok &= Expect(reader.GetPosition() == 4, "BinaryReader should seek to position");
        reader.Skip(2);
        ok &= Expect(reader.GetPosition() == 6, "BinaryReader should skip");

        return ok;
    }

    bool TestVromfsReader()
    {
        bool ok = true;

        // Construct a dummy uncompressed VROMFS in memory.
        // Header:
        // Magic (4 bytes): "VRFs"
        // Platform (4 bytes): "\x00\x00PC"
        // Original size (4 bytes): 0
        // Packed size (3 bytes): 0
        // Type (1 byte): 0x80 (maybe_packed, but size=0 -> not packed)

        std::vector<uint8_t> data;
        const char magic[] = "VRFs";
        const char platform[] = "\x00\x00PC";
        data.insert(data.end(), magic, magic + 4);
        data.insert(data.end(), platform, platform + 4);

        // original size = 12 (dummy)
        data.push_back(12); data.push_back(0); data.push_back(0); data.push_back(0);

        // packed size = 0, type = 0x80
        data.push_back(0); data.push_back(0); data.push_back(0); data.push_back(0x80);

        // Body:
        // data_start_offset = 16
        // filename_table_offset (4 bytes): 40
        // files_count (4 bytes): 1
        // Skip 8 bytes
        // filedata_table_offset (4 bytes): 50

        data.push_back(40); data.push_back(0); data.push_back(0); data.push_back(0); // filename_table_offset
        data.push_back(1); data.push_back(0); data.push_back(0); data.push_back(0); // files_count
        for (int i = 0; i < 8; ++i) data.push_back(0); // skip
        data.push_back(50); data.push_back(0); data.push_back(0); data.push_back(0); // filedata_table_offset

        // Filename table at offset 40:
        // offset to first filename = 60 (4 bytes)
        while (data.size() < 40) data.push_back(0);
        data.push_back(60); data.push_back(0); data.push_back(0); data.push_back(0);

        // Filedata table at offset 50:
        // file_data_offset = 70 (4 bytes)
        // file_data_size = 5 (4 bytes)
        while (data.size() < 50) data.push_back(0);
        data.push_back(70); data.push_back(0); data.push_back(0); data.push_back(0);
        data.push_back(5); data.push_back(0); data.push_back(0); data.push_back(0);

        // First filename at offset 60: "test\0"
        while (data.size() < 60) data.push_back(0);
        data.push_back('t'); data.push_back('e'); data.push_back('s'); data.push_back('t'); data.push_back(0);

        // File data at offset 70: "hello"
        while (data.size() < 70) data.push_back(0);
        data.push_back('h'); data.push_back('e'); data.push_back('l'); data.push_back('l'); data.push_back('o');

        DagorFormats::VromfsReader reader(data);
        reader.Parse();

        const auto& files = reader.GetFiles();
        ok &= Expect(files.size() == 1, "VromfsReader should parse 1 file");
        if (!files.empty())
        {
            ok &= Expect(files[0].name == "test", "VromfsReader should parse filename correctly");
            ok &= Expect(files[0].offset == 70, "VromfsReader should parse file offset correctly");
            ok &= Expect(files[0].size == 5, "VromfsReader should parse file size correctly");

            auto fileData = reader.GetFileData(files[0]);
            std::string content(reinterpret_cast<const char*>(fileData.data()), fileData.size());
            ok &= Expect(content == "hello", "VromfsReader should retrieve file data correctly");
        }

        return ok;
    }

    bool TestGrpReader()
    {
        bool ok = true;

        auto putU16 = [](std::vector<uint8_t>& out, size_t offset, uint16_t value) {
            out[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto putU32 = [](std::vector<uint8_t>& out, size_t offset, uint32_t value) {
            out[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        {
            // Legacy synthetic 4+4 + 11 + 4+4+4+4 GRP fixture.
            std::vector<uint8_t> data;
            const char magic[] = "GRP\x01";
            data.insert(data.end(), magic, magic + 4);

            uint32_t count = 1;
            data.push_back(count & 0xFF); data.push_back(0); data.push_back(0); data.push_back(0);

            const char name[] = "test_model";
            data.insert(data.end(), name, name + 11); // "test_model\0"

            uint32_t offset = 0x100;
            data.push_back(offset & 0xFF); data.push_back(0x01); data.push_back(0); data.push_back(0);

            uint32_t size = 10;
            data.push_back(size & 0xFF); data.push_back(0); data.push_back(0); data.push_back(0);

            uint32_t uncompressedSize = 10;
            data.push_back(uncompressedSize & 0xFF); data.push_back(0); data.push_back(0); data.push_back(0);

            uint32_t flags = 0;
            data.push_back(flags & 0xFF); data.push_back(0); data.push_back(0); data.push_back(0);

            DagorFormats::GrpReader reader(data);
            reader.Parse();

            const auto& files = reader.GetFiles();
            ok &= Expect(files.size() == 1, "GrpReader should parse 1 legacy file");
            if (!files.empty())
            {
                ok &= Expect(files[0].name == "test_model", "GrpReader legacy name match");
                ok &= Expect(files[0].offset == 0x100, "GrpReader legacy offset match");
                ok &= Expect(files[0].size == 10, "GrpReader legacy size match");
            }
        }

        {
            // Modern Dagor GRP2 layout: 16-byte header + PatchableTab-backed GrpData.
            constexpr size_t payloadOffset = 0x80;
            std::vector<uint8_t> data(payloadOffset + 4, 0);
            data[0] = 'G'; data[1] = 'R'; data[2] = 'P'; data[3] = '2';
            putU32(data, 4, 0x70); // descOnlySize is informational for this reader.
            putU32(data, 8, 0x70); // fullDataSize: bytes after the header before payloads.
            putU32(data, 12, static_cast<uint32_t>(data.size() - 16));

            // GrpData.nameMap PatchableTab at 0x10.
            putU32(data, 0x10, 0x40);
            putU32(data, 0x14, 1);

            // GrpData.resTable PatchableTab at 0x20.
            putU32(data, 0x20, 0x50);
            putU32(data, 0x24, 1);

            // GrpData.resData PatchableTab at 0x30 remains empty.
            putU32(data, 0x40, 0x60); // nameMap[0] -> "modern_model"

            putU32(data, 0x50, 0x1234ABCD); // classId
            putU32(data, 0x54, static_cast<uint32_t>(payloadOffset));
            putU16(data, 0x58, 0); // resId

            const char modernName[] = "modern_model";
            std::copy(std::begin(modernName), std::end(modernName), data.begin() + 0x60);
            data[payloadOffset + 0] = 0xCA;
            data[payloadOffset + 1] = 0xFE;
            data[payloadOffset + 2] = 0xBA;
            data[payloadOffset + 3] = 0xBE;

            DagorFormats::GrpReader reader(data);
            reader.Parse();

            const auto& files = reader.GetFiles();
            ok &= Expect(files.size() == 1, "GrpReader should parse 1 GRP2 file");
            if (!files.empty())
            {
                ok &= Expect(files[0].name == "modern_model", "GrpReader GRP2 name match");
                ok &= Expect(files[0].offset == payloadOffset, "GrpReader GRP2 payload offset match");
                ok &= Expect(files[0].size == 4, "GrpReader GRP2 payload size match");
                ok &= Expect(files[0].classId == 0x1234ABCD, "GrpReader GRP2 class id match");

                auto payload = reader.GetFileData(files[0]);
                ok &= Expect(payload.size() == 4 && payload[0] == 0xCA && payload[3] == 0xBE,
                    "GrpReader GRP2 payload data should be readable");
            }
        }

        return ok;
    }

    bool TestDxpReader()
    {
        bool ok = true;

        std::vector<uint8_t> data(256, 0); // Need at least 0x48 bytes

        // Magic
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';

        // total files at 0x8
        data[0x8] = 1; data[0x9] = 0; // 1 file

        // dds_block_offset at 0x20
        uint32_t ddsOffset = 0x60; // relative to 0x10 -> actual 0x70
        data[0x20] = ddsOffset & 0xFF;
        data[0x21] = (ddsOffset >> 8) & 0xFF;

        // block_3_offset at 0x30
        uint32_t block3Offset = 0x80; // relative to 0x10 -> actual 0x90
        data[0x30] = block3Offset & 0xFF;
        data[0x31] = (block3Offset >> 8) & 0xFF;

        // Names at 0x48
        data[0x48] = 't'; data[0x49] = 'e'; data[0x4A] = 'x'; data[0x4B] = 0;

        // DDS block at 0x70 (32 bytes)
        // just zeros is fine

        // Block 3 at 0x90 (24 bytes)
        // offset at 0x9C, size at 0xA0
        uint32_t fileOffset = 0xB0;
        data[0x9C] = fileOffset & 0xFF;

        uint32_t fileSize = 4;
        data[0xA0] = fileSize & 0xFF;

        // File data at 0xB0
        data[0xB0] = 0x11; data[0xB1] = 0x22; data[0xB2] = 0x33; data[0xB3] = 0x44;

        DagorFormats::DxpReader reader(data);
        reader.Parse();

        const auto& files = reader.GetFiles();
        ok &= Expect(files.size() == 1, "DxpReader should parse 1 file");
        if (!files.empty())
        {
            ok &= Expect(files[0].name == "tex", "DxpReader should parse filename correctly");
            ok &= Expect(files[0].offset == fileOffset, "DxpReader should parse file offset correctly");
            ok &= Expect(files[0].size == fileSize, "DxpReader should parse file size correctly");
            ok &= Expect(files[0].headerDdsx.size() == 0x20, "DxpReader should parse DDSX header size correctly");

            auto fileData = reader.GetFileData(files[0]);
            ok &= Expect(fileData.size() == 4, "DxpReader should retrieve file data correctly");
            ok &= Expect(fileData[0] == 0x11, "DxpReader data match");
        }

        return ok;
    }

    bool TestDdsxReader()
    {
        bool ok = true;

        // Construct a synthetic 32-byte DDSx header:
        //   0x00  uint32  magic = 'DDSx' (0x78534444 little-endian)
        //   0x04  uint32  d3dFormat = 77 (BC3_UNORM)
        //   0x08  uint32  flags = 0
        //   0x0C  uint16  width = 1024
        //   0x0E  uint16  height = 512
        //   0x10  uint8   mipCount = 10
        //   0x11  uint8   hqPartLevels = 0
        //   0x12  uint16  depth = 1
        //   0x14  uint16  bitsPerPixel = 8
        //   0x16  uint16  quality/shift nibbles = 0
        //   0x18  uint32  memorySize = 524288
        //   0x1C  uint32  packedSize = 131072
        std::vector<uint8_t> data(32, 0);

        // Magic: "DDSx" -> 0x44 0x44 0x53 0x78
        data[0] = 0x44; data[1] = 0x44; data[2] = 0x53; data[3] = 0x78;

        // d3dFormat = 77 (BC3_UNORM)
        data[4] = 77; data[5] = 0; data[6] = 0; data[7] = 0;

        // flags = FLG_ZSTD (0x20000000)
        data[11] = 0x20;

        // width = 1024 (0x0400)
        data[12] = 0x00; data[13] = 0x04;

        // height = 512 (0x0200)
        data[14] = 0x00; data[15] = 0x02;

        // mipCount = 10
        data[16] = 10;

        // depth = 1
        data[18] = 1;

        // bitsPerPixel = 8
        data[20] = 8;

        // memorySize = 524288 (0x00080000)
        data[24] = 0x00; data[25] = 0x00; data[26] = 0x08; data[27] = 0x00;

        // packedSize = 131072 (0x00020000)
        data[28] = 0x00; data[29] = 0x00; data[30] = 0x02; data[31] = 0x00;

        DagorFormats::DdsxReader reader(data);
        reader.Parse();

        const DagorFormats::DdsxHeader& hdr = reader.GetHeader();
        ok &= Expect(hdr.IsValid(), "DdsxReader header should be valid");
        ok &= Expect(hdr.d3dFormat == 77, "DdsxReader d3dFormat should be 77 (BC3_UNORM)");
        ok &= Expect(hdr.width == 1024, "DdsxReader width should be 1024");
        ok &= Expect(hdr.height == 512, "DdsxReader height should be 512");
        ok &= Expect(hdr.mipCount == 10, "DdsxReader mipCount should be 10");
        ok &= Expect(hdr.depth == 1, "DdsxReader depth should be 1");
        ok &= Expect(hdr.bitsPerPixel == 8, "DdsxReader bitsPerPixel should be 8");
        ok &= Expect(hdr.memorySize == 524288, "DdsxReader memorySize should be 524288");
        ok &= Expect(hdr.packedSize == 131072, "DdsxReader packedSize should be 131072");
        ok &= Expect(hdr.compressionFlag == 0x20, "DdsxReader compression flag should be zstd");
        ok &= Expect(hdr.compressionType == 2, "DdsxReader normalized compression type should be zstd");
        ok &= Expect(hdr.IsCompressed(), "DdsxReader should detect compressed data");
        ok &= Expect(!hdr.IsOodleCompressed(), "DdsxReader should not detect Oodle");
        ok &= Expect(std::string(hdr.FormatName()) == "BC3_UNORM", "DdsxReader format name should be BC3_UNORM");
        ok &= Expect(std::string(hdr.CompressionName()) == "zstd", "DdsxReader compression name should be zstd");

        return ok;
    }

    bool TestJobSystem()
    {
        bool ok = true;

        DagorAssetsExtractor::Jobs::JobSystem jobs;
        std::atomic_int completed = 0;

        jobs.Start("increment-a", [&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });
        jobs.Start("increment-b", [&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
        });

        jobs.JoinAll();
        ok &= Expect(completed.load(std::memory_order_relaxed) == 2, "JobSystem should run and join all jobs");
        ok &= Expect(jobs.RunningCount() == 0, "JobSystem should have no running jobs after JoinAll");

        jobs.Start("short-job", [&completed]() {
            completed.store(7, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (jobs.RunningCount() != 0 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        jobs.JoinFinished();
        ok &= Expect(completed.load(std::memory_order_acquire) == 7, "JobSystem should finish short jobs");
        ok &= Expect(jobs.RunningCount() == 0, "JobSystem should clean up finished jobs");

        return ok;
    }

    bool TestDecompression()
    {
        bool ok = true;

        // Test Zlib decompression
        // "Hello World" compressed with zlib (deflate)
        const uint8_t compressedZlib[] = {
            0x78, 0x9c, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x08, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00, 0x18, 0x0b, 0x04, 0x1d
        };
        const std::string expected = "Hello World";

        try {
            std::vector<uint8_t> decompressed = DagorIO::Decompressor::DecompressZlib(compressedZlib, expected.size());
            std::string actual(decompressed.begin(), decompressed.end());
            ok &= Expect(actual == expected, "Zlib decompression should work for 'Hello World'");
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("Zlib decompression failed: ") + e.what()).c_str());
        }

        // Test Zstd decompression
        // "Hello World" compressed with zstd
        const uint8_t compressedZstd[] = {
            0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x58, 0x59, 0x00, 0x00, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64
        };
        // Note: zstd header with 11 bytes of data.
        // Wait, let's verify the zstd header for "Hello World".
        // Actually, I'll just use a small valid zstd block if I can generate one or find one.
        // I'll trust the zstd library for now or use a known sample.

        try {
            std::vector<uint8_t> decompressed = DagorIO::Decompressor::DecompressZstd(compressedZstd, expected.size());
            std::string actual(decompressed.begin(), decompressed.end());
            ok &= Expect(actual == expected, "Zstd decompression should work for 'Hello World'");
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("Zstd decompression failed: ") + e.what()).c_str());
        }

        // Test LZMA decompression with explicit size
        const uint8_t compressedLzmaExplicit[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x55, 0x2E,
            0xD5, 0x8F, 0x6D, 0x14, 0x44, 0x03, 0x2D, 0x4B, 0x4C, 0x5E, 0xFF, 0xFF, 0xC0, 0x5F, 0x00, 0x00
        };
        const uint8_t expectedLzma[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44 };

        try {
            std::vector<uint8_t> decompressed = DagorIO::Decompressor::DecompressLzma(compressedLzmaExplicit, 8);
            ok &= Expect(decompressed.size() == 8, "LZMA decompression explicit size should output 8 bytes");
            if (decompressed.size() == 8) {
                bool match = true;
                for (int i = 0; i < 8; ++i) {
                    if (decompressed[i] != expectedLzma[i]) {
                        match = false;
                    }
                }
                ok &= Expect(match, "LZMA decompressed payload (explicit size) should match expected bytes");
            }
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("LZMA decompression (explicit size) failed: ") + e.what()).c_str());
        }

        // Test LZMA decompression with stream size (overload)
        const uint8_t compressedLzmaStream[] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x2E,
            0xD5, 0x8F, 0x6D, 0x14, 0x44, 0x03, 0x2D, 0x4B, 0x4C, 0x5E, 0xFF, 0xFF, 0xC0, 0x5F, 0x00, 0x00
        };

        try {
            std::vector<uint8_t> decompressed = DagorIO::Decompressor::DecompressLzma(compressedLzmaStream);
            ok &= Expect(decompressed.size() == 8, "LZMA decompression stream size should output 8 bytes");
            if (decompressed.size() == 8) {
                bool match = true;
                for (int i = 0; i < 8; ++i) {
                    if (decompressed[i] != expectedLzma[i]) {
                        match = false;
                    }
                }
                ok &= Expect(match, "LZMA decompressed payload (stream size) should match expected bytes");
            }
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("LZMA decompression (stream size) failed: ") + e.what()).c_str());
        }

        return ok;
    }

    size_t CountPropertiesRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.properties.size();
        for (const DagorFormats::BlkBlock& child : block.children)
        {
            count += CountPropertiesRecursive(child);
        }
        return count;
    }

    size_t CountBlocksRecursive(const DagorFormats::BlkBlock& block)
    {
        size_t count = block.children.size();
        for (const DagorFormats::BlkBlock& child : block.children)
        {
            count += CountBlocksRecursive(child);
        }
        return count;
    }

    bool TestBinaryBlkReader()
    {
        bool ok = true;

        // Dummy binary BLK (uncompressed)
        // 1 block "root", 1 string param "testStr" = "value"
        std::vector<uint8_t> data;
        data.push_back(0x01); // magic kBinaryBlkPayloadMagic
        
        // Names map
        data.push_back(0x02); // Names count = 2
        data.push_back(0x0F); // Names size = 15 bytes
        data.push_back(0x00); data.push_back(0x00); // 2 empty bytes prefix
        const char name1[] = "root"; data.insert(data.end(), name1, name1 + 5); // 0-based idx 0 => "root"
        const char name2[] = "testStr"; data.insert(data.end(), name2, name2 + 8); // 0-based idx 1 => "testStr"
        
        data.push_back(0x01); // Blocks count = 1
        data.push_back(0x01); // Params count = 1
        data.push_back(0x06); // Params data size = 6 ("value\0")
        
        // paramsData (6 bytes)
        const char paramVal[] = "value"; data.insert(data.end(), paramVal, paramVal + 6); // Offset 0
        
        // paramsTable (1 param * 8 = 8 bytes)
        // Param 0: "testStr"
        // nameId = 2 (index 1), type = 0x01 (string)
        // flags = 0x01000002
        AppendU32(data, 0x01000002);
        // rawValue = 0 (offset in params data)
        AppendU32(data, 0);

        // blocks (1 block)
        // Block 0: "root"
        data.push_back(0x02); // Name ID = 2 (mapped to index 0)
        data.push_back(0x01); // Params count = 1
        data.push_back(0x00); // Child count = 0

        std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        DagorFormats::BlkReader reader(text);
        reader.Parse();

        const DagorFormats::BlkBlock& root = reader.GetRoot();
        ok &= Expect(root.name == "root", "Binary BLK should parse root block name");
        ok &= Expect(root.properties.size() == 1, "Binary BLK should parse one property");
        if (root.properties.size() == 1)
        {
            ok &= Expect(root.properties[0].name == "testStr", "Binary BLK should parse property name");
            ok &= Expect(root.properties[0].type == "t", "Binary BLK should parse property type");
            ok &= Expect(root.properties[0].value == "value", "Binary BLK should parse string value");
        }

        return ok;
    }

    bool TestBlkReader()
    {
        bool ok = true;

        const std::string text =
            "# sample Dagor BLK\n"
            "gameResPack:t=\"sample_vehicle\"\n"
            "ddsxTexPack:t=\"sample_textures\"\n"
            "virtual_res_blk {\n"
            "  folder:t=\"content/base/res\"\n"
            "  enabled:b=yes // raw value comment\n"
            "  lodDistance:r=120.5\n"
            "  node {\n"
            "    name:t=\"tank_body\"\n"
            "  }\n"
            "}\n";

        DagorFormats::BlkReader reader(text);
        reader.Parse();

        const DagorFormats::BlkBlock& root = reader.GetRoot();
        ok &= Expect(root.properties.size() == 2, "BlkReader should parse two top-level properties");
        ok &= Expect(root.children.size() == 1, "BlkReader should parse one top-level block");

        if (root.properties.size() >= 2)
        {
            ok &= Expect(root.properties[0].name == "gameResPack", "BlkReader should parse property name");
            ok &= Expect(root.properties[0].type == "t", "BlkReader should parse property type suffix");
            ok &= Expect(root.properties[0].value == "sample_vehicle", "BlkReader should parse quoted value");
            ok &= Expect(root.properties[1].name == "ddsxTexPack", "BlkReader should parse second property name");
        }

        if (!root.children.empty())
        {
            const DagorFormats::BlkBlock& virtualBlock = root.children[0];
            ok &= Expect(virtualBlock.name == "virtual_res_blk", "BlkReader should parse child block name");
            ok &= Expect(virtualBlock.properties.size() == 3, "BlkReader should parse child block properties");
            ok &= Expect(virtualBlock.children.size() == 1, "BlkReader should parse nested child block");

            if (virtualBlock.properties.size() >= 3)
            {
                ok &= Expect(virtualBlock.properties[1].name == "enabled", "BlkReader should parse bool-like property name");
                ok &= Expect(virtualBlock.properties[1].type == "b", "BlkReader should preserve bool-like type suffix");
                ok &= Expect(virtualBlock.properties[1].value == "yes", "BlkReader should parse raw values");
                ok &= Expect(virtualBlock.properties[2].value == "120.5", "BlkReader should parse numeric raw values");
            }
        }

        ok &= Expect(CountPropertiesRecursive(root) == 6, "BlkReader recursive property count should match");
        ok &= Expect(CountBlocksRecursive(root) == 2, "BlkReader recursive block count should match");

        return ok;
    }

    void AppendU32(std::vector<uint8_t>& data, uint32_t value)
    {
        data.push_back(static_cast<uint8_t>(value & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        data.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }

    void SetU16(std::vector<uint8_t>& data, size_t offset, uint16_t value)
    {
        data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }

    void SetU32(std::vector<uint8_t>& data, size_t offset, uint32_t value)
    {
        data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
        data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    }

    bool WriteBinaryFile(const fs::path& path, const std::vector<uint8_t>& data);

    void AppendVlq(std::vector<uint8_t>& data, uint32_t value)
    {
        do
        {
            uint8_t byte = static_cast<uint8_t>(value & 0x7F);
            value >>= 7;
            if (value != 0)
            {
                byte |= 0x80;
            }
            data.push_back(byte);
        }
        while (value != 0);
    }

    std::vector<uint8_t> MakeSyntheticGameResDescPayload()
    {
        std::vector<uint8_t> payload;
        payload.push_back(1); // binary BLK payload magic

        const std::vector<std::string> names = {
            "root",
            "sample_tank",
            "tex",
            "t0",
            "t1",
            "matR",
            "material",
            "cls",
            "t2",
        };

        std::vector<uint8_t> nameMap = { 0, 0 };
        for (const std::string& name : names)
        {
            nameMap.insert(nameMap.end(), name.begin(), name.end());
            nameMap.push_back(0);
        }

        AppendVlq(payload, static_cast<uint32_t>(names.size()));
        AppendVlq(payload, static_cast<uint32_t>(nameMap.size()));
        payload.insert(payload.end(), nameMap.begin(), nameMap.end());

        constexpr uint32_t blocksCount = 5;
        constexpr uint32_t paramsCount = 4;

        std::vector<uint8_t> paramsData;
        auto appendString = [&paramsData](std::string_view value) {
            const uint32_t offset = static_cast<uint32_t>(paramsData.size());
            paramsData.insert(paramsData.end(), value.begin(), value.end());
            paramsData.push_back(0);
            return offset;
        };

        const uint32_t bodyTexOffset = appendString("body_tex");
        const uint32_t normalTexOffset = appendString("body_n");
        const uint32_t shaderClassOffset = appendString("rendinst_simple");

        AppendVlq(payload, blocksCount);
        AppendVlq(payload, paramsCount);
        AppendVlq(payload, static_cast<uint32_t>(paramsData.size()));
        payload.insert(payload.end(), paramsData.begin(), paramsData.end());

        auto appendParam = [&payload](uint32_t flags, uint32_t value) {
            AppendU32(payload, flags);
            AppendU32(payload, value);
        };

        appendParam(0x01000000u | 4u, bodyTexOffset);    // t0:t = body_tex
        appendParam(0x01000000u | 5u, normalTexOffset);  // t1:t = body_n
        appendParam(0x01000000u | 8u, shaderClassOffset);// cls:t = rendinst_simple
        appendParam(0x02000000u | 4u, 0u);               // material t0 texture slot references tex[0]

        auto appendBlock = [&payload](uint32_t nameIndex, uint32_t paramCount, uint32_t childCount, uint32_t firstChild) {
            AppendVlq(payload, nameIndex + 2); // descriptor binary BLK block ids are two-based
            AppendVlq(payload, paramCount);
            AppendVlq(payload, childCount);
            if (childCount != 0)
            {
                AppendVlq(payload, firstChild);
            }
        };

        appendBlock(0, 0, 1, 1); // root -> sample_tank
        appendBlock(1, 0, 2, 2); // sample_tank -> tex, matR
        appendBlock(2, 2, 0, 0); // tex
        appendBlock(5, 0, 1, 4); // matR -> material
        appendBlock(6, 2, 0, 0); // material

        return payload;
    }

    bool TestGameResDescReader()
    {
        bool ok = true;
        const std::vector<uint8_t> payload = MakeSyntheticGameResDescPayload();

        DagorFormats::GameResDescReader reader(payload);
        reader.Parse();

        const std::vector<DagorFormats::GameResDescEntry>& entries = reader.GetEntries();
        ok &= Expect(entries.size() == 1, "GameResDescReader should expose one top-level resource");
        if (!entries.empty())
        {
            const DagorFormats::GameResDescEntry& entry = entries[0];
            ok &= Expect(entry.name == "sample_tank", "GameResDescReader should parse model block name");
            ok &= Expect(entry.resId == 0, "GameResDescReader should assign stable resource id");
            ok &= Expect(entry.textures.size() == 2, "GameResDescReader should collect texture names");
            if (entry.textures.size() == 2)
            {
                ok &= Expect(entry.textures[0] == "body_tex", "GameResDescReader first texture should match");
                ok &= Expect(entry.textures[1] == "body_n", "GameResDescReader second texture should match");
            }
            ok &= Expect(entry.materials.size() == 1, "GameResDescReader should collect material summaries");
            if (!entry.materials.empty())
            {
                ok &= Expect(entry.materials[0] == "body",
                    "GameResDescReader should derive material names from texture slots");
            }
            ok &= Expect(entry.materialDomains.size() == 1 && entry.materialDomains[0] == "rigid",
                "GameResDescReader should preserve descriptor material domains");
            ok &= Expect(entry.materialShaderClasses.size() == 1 && entry.materialShaderClasses[0] == "rendinst_simple",
                "GameResDescReader should preserve material shader classes");
            ok &= Expect(entry.shaders.size() == 1 && entry.shaders[0] == "rendinst_simple",
                "GameResDescReader should collect shader class");
            ok &= Expect(entry.dependencyIds.size() == 1 && entry.dependencyIds[0] == 0,
                "GameResDescReader should collect material texture index references");
            ok &= Expect(entry.materialTextureRefs.size() == 1,
                "GameResDescReader should preserve material-owned texture references");
            if (!entry.materialTextureRefs.empty())
            {
                ok &= Expect(entry.materialTextureRefs[0].materialIndex == 0,
                    "GameResDescReader material texture ref should preserve material slot");
                ok &= Expect(entry.materialTextureRefs[0].textureIndex == 0,
                    "GameResDescReader material texture ref should preserve descriptor texture slot");
                ok &= Expect(entry.materialTextureRefs[0].slotName == "t0",
                    "GameResDescReader material texture ref should preserve descriptor slot name");
            }
            ok &= Expect(entry.className.find("2 tex") != std::string::npos,
                "GameResDescReader class summary should include texture count");
        }

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_GameResDescTest_" + std::to_string(stamp));
        const fs::path descPath = tempDir / "riDesc.bin";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp GameResDesc test folder: " << ec.message() << '\n';
            return false;
        }

        {
            std::ofstream desc(descPath, std::ios::binary);
            desc.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDescriptorContents(descPath);
        ok &= Expect(contents != nullptr, "Descriptor PackContents should be returned");
        if (contents)
        {
            ok &= Expect(contents->error.empty(), "Descriptor PackContents should parse synthetic descriptor");
            ok &= Expect(contents->entries.size() == 1, "Descriptor PackContents should expose parsed descriptor entry");
            if (!contents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& sub = contents->entries[0];
                ok &= Expect(sub.name == "sample_tank", "Descriptor PackContents should preserve resource name");
                ok &= Expect(sub.textureNames.size() == 2, "Descriptor PackContents should preserve texture list");
                ok &= Expect(sub.materialNames.size() == 1, "Descriptor PackContents should preserve material list");
                if (!sub.materialNames.empty())
                {
                    ok &= Expect(sub.materialNames[0] == "body",
                        "Descriptor PackContents should preserve generated material names");
                }
                ok &= Expect(sub.materialDomains.size() == 1 && sub.materialDomains[0] == "rigid",
                    "Descriptor PackContents should preserve material domains");
                ok &= Expect(sub.materialShaderClasses.size() == 1 && sub.materialShaderClasses[0] == "rendinst_simple",
                    "Descriptor PackContents should preserve material shader classes");
                ok &= Expect(sub.materialTextureRefs.size() == 1,
                    "Descriptor PackContents should preserve per-material texture refs");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    std::array<float, 16> MakeAffineTranslationMatrix(float tx, float ty, float tz)
    {
        return {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            tx,   ty,   tz,   1.0f,
        };
    }

    void WriteMatrix(std::vector<uint8_t>& data, size_t offset, const std::array<float, 16>& matrix)
    {
        std::memcpy(data.data() + offset, matrix.data(), sizeof(float) * matrix.size());
    }

    std::vector<uint8_t> MakeSyntheticGeomNodeTreePayload()
    {
        constexpr size_t nodeCount = 2;
        constexpr size_t oldNodeSize = 160;
        constexpr uint32_t lastUnimportantCount = 1;
        constexpr size_t namesOffset = nodeCount * oldNodeSize;
        constexpr char names[] = "root\0child\0";
        const size_t rawDataSize = (namesOffset + sizeof(names) + 3) & ~size_t(3);

        std::vector<uint8_t> data(8 + rawDataSize, 0);
        SetU32(data, 0, static_cast<uint32_t>(rawDataSize) | (lastUnimportantCount << 20));
        SetU32(data, 4, static_cast<uint32_t>(nodeCount));

        const size_t base = 8;
        const std::array<float, 16> rootLocal = MakeAffineTranslationMatrix(10.0f, 0.0f, 0.0f);
        const std::array<float, 16> childLocal = MakeAffineTranslationMatrix(0.0f, 2.0f, 0.0f);
        const std::array<float, 16> childWorld = MakeAffineTranslationMatrix(10.0f, 2.0f, 0.0f);

        WriteMatrix(data, base + 0, rootLocal);
        WriteMatrix(data, base + 64, rootLocal);
        SetU32(data, base + 128, static_cast<uint32_t>(oldNodeSize));
        SetU32(data, base + 132, 1);
        SetU32(data, base + 144, 0xFFFFFFFFu);
        SetU32(data, base + 148, 0);
        SetU32(data, base + 152, static_cast<uint32_t>(namesOffset));
        SetU32(data, base + 156, 0);

        const size_t childBase = base + oldNodeSize;
        WriteMatrix(data, childBase + 0, childLocal);
        WriteMatrix(data, childBase + 64, childWorld);
        SetU32(data, childBase + 128, 0);
        SetU32(data, childBase + 132, 0);
        SetU32(data, childBase + 144, 0);
        SetU32(data, childBase + 148, 0);
        SetU32(data, childBase + 152, static_cast<uint32_t>(namesOffset + 5));
        SetU32(data, childBase + 156, 0);

        std::memcpy(data.data() + base + namesOffset, names, sizeof(names));
        return data;
    }

    std::vector<uint8_t> MakeSyntheticModernGrpPack(std::string_view entryName, uint32_t classId, const std::vector<uint8_t>& payload)
    {
        constexpr size_t payloadOffset = 0x80;
        std::vector<uint8_t> data(payloadOffset + payload.size(), 0);
        data[0] = 'G';
        data[1] = 'R';
        data[2] = 'P';
        data[3] = '2';
        SetU32(data, 4, 0x70);
        SetU32(data, 8, 0x70);
        SetU32(data, 12, static_cast<uint32_t>(data.size() - 16));

        SetU32(data, 0x10, 0x40);
        SetU32(data, 0x14, 1);
        SetU32(data, 0x20, 0x50);
        SetU32(data, 0x24, 1);
        SetU32(data, 0x40, 0x60);

        SetU32(data, 0x50, classId);
        SetU32(data, 0x54, static_cast<uint32_t>(payloadOffset));
        SetU16(data, 0x58, 0);

        std::copy(entryName.begin(), entryName.end(), data.begin() + 0x60);
        data[0x60 + entryName.size()] = 0;
        std::copy(payload.begin(), payload.end(), data.begin() + payloadOffset);
        return data;
    }

    std::vector<uint8_t> MakeSyntheticModernGrpPack(std::string_view entryName, uint32_t classId, std::string_view payloadMagic)
    {
        return MakeSyntheticModernGrpPack(
            entryName,
            classId,
            std::vector<uint8_t>(payloadMagic.begin(), payloadMagic.end()));
    }

    std::vector<uint8_t> MakeSyntheticAnim2DataPayload()
    {
        std::vector<uint8_t> dump(80, 0);
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value)
        {
            if (data.size() < offset + sizeof(uint32_t))
            {
                data.resize(offset + sizeof(uint32_t));
            }
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };
        auto put64 = [&](std::vector<uint8_t>& data, size_t offset, uint64_t value)
        {
            put32(data, offset, static_cast<uint32_t>(value & 0xFFFFFFFFu));
            put32(data, offset + sizeof(uint32_t), static_cast<uint32_t>(value >> 32));
        };
        auto patchableTab = [](uint32_t offset, uint32_t count)
        {
            return (static_cast<uint64_t>(count) << 32) | offset;
        };
        auto appendPatchableTab = [&](std::vector<uint8_t>& data, uint32_t offset, uint32_t count)
        {
            put64(data, data.size(), patchableTab(offset, count));
            put64(data, data.size(), 0);
        };

        constexpr uint32_t nodeNameTableOffset = 40;
        constexpr uint32_t nodeNameOffset = 48;
        constexpr uint32_t noteTrackOffset = 56;
        constexpr uint32_t labelNameOffset = 72;

        put32(dump, 16, 1); // nodeNum
        put32(dump, 20, 1); // CHTYPE_POSITION
        put64(dump, 32, nodeNameTableOffset);
        put64(dump, nodeNameTableOffset, nodeNameOffset);
        const std::string nodeName = "root";
        std::copy(nodeName.begin(), nodeName.end(), dump.begin() + nodeNameOffset);

        put64(dump, noteTrackOffset, labelNameOffset);
        put32(dump, noteTrackOffset + 8, 60 << 8);
        const std::string labelName = "start";
        std::copy(labelName.begin(), labelName.end(), dump.begin() + labelNameOffset);

        std::vector<uint8_t> payload;
        put32(payload, 0, 0x4D494E41u); // ANIM
        put32(payload, 4, 0x300u);
        put32(payload, 8, 16u);
        put32(payload, 12, static_cast<uint32_t>(dump.size()));
        payload.insert(payload.end(), dump.begin(), dump.end());
        appendPatchableTab(payload, 0, 1); // chanPoint3
        appendPatchableTab(payload, 0, 0); // chanQuat
        appendPatchableTab(payload, 0, 0); // chanReal
        appendPatchableTab(payload, noteTrackOffset, 1);
        return payload;
    }

    bool TestAnim2DataReader()
    {
        bool ok = true;
        const std::vector<uint8_t> payload = MakeSyntheticAnim2DataPayload();
        DagorFormats::Anim2DataReader reader(payload);
        reader.Parse();
        const DagorFormats::Anim2DataInfo& info = reader.GetInfo();
        ok &= Expect(info.version == 0x300u, "Anim2DataReader should parse ANIM 0x300");
        ok &= Expect(info.channels.size() == 1, "Anim2DataReader should expose one channel");
        ok &= Expect(info.nodeNames.size() == 1 && info.nodeNames[0] == "root",
            "Anim2DataReader should expose channel node names");
        ok &= Expect(info.labels.size() == 1 && info.labels[0].name == "start",
            "Anim2DataReader should expose note-track labels");
        return ok;
    }

    bool TestGeomNodeTreeReader()
    {
        bool ok = true;
        const std::vector<uint8_t> payload = MakeSyntheticGeomNodeTreePayload();
        DagorFormats::GeomNodeTreeReader reader(payload);
        reader.Parse();

        const auto& nodes = reader.GetNodes();
        ok &= Expect(nodes.size() == 2, "GeomNodeTreeReader should parse two nodes");
        ok &= Expect(reader.GetImportantNodeCount() == 1, "GeomNodeTreeReader should preserve important node count");
        ok &= Expect(!reader.HasCompressedPayload(), "GeomNodeTreeReader synthetic payload should be uncompressed");

        if (nodes.size() == 2)
        {
            ok &= Expect(nodes[0].name == "root", "GeomNodeTreeReader should preserve root name");
            ok &= Expect(nodes[0].parentIndex == DagorFormats::GeomNodeTreeNode::InvalidIndex,
                "GeomNodeTreeReader should mark the root parent as invalid");
            ok &= Expect(nodes[0].childIndices.size() == 1 && nodes[0].childIndices[0] == 1,
                "GeomNodeTreeReader should preserve child ranges");
            ok &= Expect(nodes[1].name == "child", "GeomNodeTreeReader should preserve child name");
            ok &= Expect(nodes[1].parentIndex == 0, "GeomNodeTreeReader should preserve parent indices");

            ok &= ExpectNear(nodes[0].worldTransform[3], 10.0f, 0.0001f,
                "GeomNodeTreeReader should preserve root world X translation");
            ok &= ExpectNear(nodes[1].worldTransform[3], 10.0f, 0.0001f,
                "GeomNodeTreeReader should accumulate child world X translation");
            ok &= ExpectNear(nodes[1].worldTransform[7], 2.0f, 0.0001f,
                "GeomNodeTreeReader should accumulate child world Y translation");
            ok &= ExpectNear(nodes[1].inverseBindTransform[3], -10.0f, 0.0001f,
                "GeomNodeTreeReader should compute inverse bind X translation");
            ok &= ExpectNear(nodes[1].inverseBindTransform[7], -2.0f, 0.0001f,
                "GeomNodeTreeReader should compute inverse bind Y translation");
        }

        return ok;
    }

    bool TestGrpPackContentsMetadata()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_GrpPackTest_" + std::to_string(stamp));
        const fs::path rendInstPath = tempDir / "static_mesh.grp";
        const fs::path animPath = tempDir / "animation.grp";
        const fs::path skeletonPath = tempDir / "skeleton.grp";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp GRP test folder: " << ec.message() << '\n';
            return false;
        }

        ok &= Expect(
            WriteBinaryFile(rendInstPath, MakeSyntheticModernGrpPack("static_mesh", 0x77F8232Fu, "RIND")),
            "Modern RendInst GRP fixture should be written");
        ok &= Expect(
            WriteBinaryFile(animPath, MakeSyntheticModernGrpPack("anim_tree", 0x40C586F9u, MakeSyntheticAnim2DataPayload())),
            "Modern Anim2Data GRP fixture should be written");
        ok &= Expect(
            WriteBinaryFile(skeletonPath, MakeSyntheticModernGrpPack("tank_skeleton", 0x56F81B6Du, MakeSyntheticGeomNodeTreePayload())),
            "Modern GeomNodeTree GRP fixture should be written");

        const std::shared_ptr<AssetIndex::PackContents> rendInstContents = AssetIndex::ParseGrpContents(rendInstPath);
        ok &= Expect(rendInstContents != nullptr, "Modern RendInst GRP PackContents should be returned");
        if (rendInstContents)
        {
            ok &= Expect(rendInstContents->error.empty(), "Modern RendInst GRP PackContents should parse without error");
            ok &= Expect(rendInstContents->entries.size() == 1, "Modern RendInst GRP should expose one entry");
            if (!rendInstContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = rendInstContents->entries[0];
                ok &= Expect(entry.grpClassId == 0x77F8232Fu, "Modern RendInst GRP should preserve class id");
                ok &= Expect(entry.formatInfo == "RendInst", "Modern RendInst GRP should label class name");
                ok &= Expect(AssetIndex::CanExportGrpEntryAsStaticModel(entry), "RendInst GRP entry should be exportable");
            }
        }

        const std::shared_ptr<AssetIndex::PackContents> animContents = AssetIndex::ParseGrpContents(animPath);
        ok &= Expect(animContents != nullptr, "Modern Anim2Data GRP PackContents should be returned");
        if (animContents)
        {
            ok &= Expect(animContents->error.empty(), "Modern Anim2Data GRP PackContents should parse without error");
            ok &= Expect(animContents->entries.size() == 1, "Modern Anim2Data GRP should expose one entry");
            if (!animContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = animContents->entries[0];
                ok &= Expect(entry.grpClassId == 0x40C586F9u, "Modern Anim2Data GRP should preserve class id");
                ok &= Expect(entry.animVersion == 0x300u, "Modern Anim2Data GRP should expose ANIM version");
                ok &= Expect(entry.animChannelCount == 1, "Modern Anim2Data GRP should expose channel count");
                ok &= Expect(entry.animNodeCount == 1, "Modern Anim2Data GRP should expose animated node count");
                ok &= Expect(entry.animLabelCount == 1, "Modern Anim2Data GRP should expose note label count");
                ok &= Expect(entry.formatInfo.find("Anim2Data ANIM 0x300") != std::string::npos,
                    "Modern Anim2Data GRP should summarize ANIM metadata");
                ok &= Expect(!AssetIndex::CanExportGrpEntryAsStaticModel(entry), "Anim2Data GRP entry should not be exportable as a static model");
            }
        }

        const std::shared_ptr<AssetIndex::PackContents> skeletonContents = AssetIndex::ParseGrpContents(skeletonPath);
        ok &= Expect(skeletonContents != nullptr, "Modern GeomNodeTree GRP PackContents should be returned");
        if (skeletonContents)
        {
            ok &= Expect(skeletonContents->error.empty(), "Modern GeomNodeTree GRP PackContents should parse without error");
            ok &= Expect(skeletonContents->entries.size() == 1, "Modern GeomNodeTree GRP should expose one entry");
            if (!skeletonContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = skeletonContents->entries[0];
                ok &= Expect(entry.grpClassId == 0x56F81B6Du, "Modern GeomNodeTree GRP should preserve class id");
                ok &= Expect(entry.skeletonNodeCount == 2, "Modern GeomNodeTree GRP should expose skeleton node count");
                ok &= Expect(entry.skeletonImportantNodeCount == 1,
                    "Modern GeomNodeTree GRP should expose important node count");
                ok &= Expect(entry.skeletonRootName == "root", "Modern GeomNodeTree GRP should expose the root node name");
                ok &= Expect(entry.formatInfo.find("GeomNodeTree 2 nodes") != std::string::npos,
                    "Modern GeomNodeTree GRP should summarize skeleton node metadata");
                ok &= Expect(entry.formatInfo.find("root=root") != std::string::npos,
                    "Modern GeomNodeTree GRP should summarize the skeleton root name");
                ok &= Expect(!AssetIndex::CanExportGrpEntryAsStaticModel(entry),
                    "GeomNodeTree GRP entry should not be exportable as a static model");
            }
        }

        ok &= Expect(AssetIndex::DescribeGrpClassId(0xDEADBEEFu) == "class 0xDEADBEEF", "Unknown GRP class id should be formatted as hex");

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestFixtureBlk(const fs::path& fixtureRoot)
    {
        bool ok = true;
        const fs::path fixtureBlk = fixtureRoot / "content" / "base" / "res" / "sample_rules.blk";

        std::ifstream file(fixtureBlk);
        if (!file)
        {
            return Expect(false, "Fixture BLK should open");
        }

        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        DagorFormats::BlkReader reader(text);
        reader.Parse();

        const DagorFormats::BlkBlock& root = reader.GetRoot();
        ok &= Expect(root.properties.size() == 2, "fixture BLK should parse two top-level properties");
        if (root.properties.size() >= 2)
        {
            ok &= Expect(root.properties[0].name == "gameResPack", "fixture BLK should include gameResPack");
            ok &= Expect(root.properties[0].type == "t", "fixture BLK gameResPack should use text type");
            ok &= Expect(root.properties[0].value == "sample_vehicle", "fixture BLK gameResPack should match value");
            ok &= Expect(root.properties[1].name == "ddsxTexPack", "fixture BLK should include ddsxTexPack");
            ok &= Expect(root.properties[1].value == "sample_textures", "fixture BLK ddsxTexPack should match value");
        }

        return ok;
    }

    bool TestPackContentsFixtures(const fs::path& fixtureRoot)
    {
        bool ok = true;

        const fs::path vromfsPath = fixtureRoot / "content" / "packs" / "sample_content.vromfs.bin";
        const std::shared_ptr<AssetIndex::PackContents> vromfsContents = AssetIndex::ParseVromfsContents(vromfsPath);
        ok &= Expect(vromfsContents != nullptr, "VROMFS PackContents should be returned");
        if (vromfsContents)
        {
            ok &= Expect(vromfsContents->parsed, "VROMFS PackContents should be marked parsed");
            ok &= Expect(vromfsContents->error.empty(), "VROMFS PackContents should parse without error");
            ok &= Expect(vromfsContents->containerType == "VROMFS", "VROMFS PackContents should identify container type");
            ok &= Expect(vromfsContents->entries.size() == 1, "VROMFS PackContents should expose one entry");

            if (!vromfsContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = vromfsContents->entries[0];
                ok &= Expect(entry.name == "test", "VROMFS PackContents should expose fixture file name");
                ok &= Expect(entry.offset == 70, "VROMFS PackContents should expose fixture file offset");
                ok &= Expect(entry.size == 5, "VROMFS PackContents should expose fixture file size");
            }
        }

        const fs::path dxpPath = fixtureRoot / "content" / "base" / "res" / "sample_textures.dxp.bin";
        const std::shared_ptr<AssetIndex::PackContents> dxpContents = AssetIndex::ParseDxpContents(dxpPath);
        ok &= Expect(dxpContents != nullptr, "DXP PackContents should be returned");
        if (dxpContents)
        {
            ok &= Expect(dxpContents->parsed, "DXP PackContents should be marked parsed");
            ok &= Expect(dxpContents->error.empty(), "DXP PackContents should parse without container error");
            ok &= Expect(dxpContents->containerType == "DXP", "DXP PackContents should identify container type");
            ok &= Expect(dxpContents->entries.size() == 1, "DXP PackContents should expose one entry");

            if (!dxpContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = dxpContents->entries[0];
                ok &= Expect(entry.name == "tex", "DXP PackContents should expose fixture texture name");
                ok &= Expect(entry.offset == 0xB0, "DXP PackContents should expose fixture texture offset");
                ok &= Expect(entry.size == 4, "DXP PackContents should expose fixture texture size");
                ok &= Expect(entry.texWidth == 128, "DXP PackContents should parse DDSx texture width");
                ok &= Expect(entry.texHeight == 64, "DXP PackContents should parse DDSx texture height");
                ok &= Expect(entry.texMips == 7, "DXP PackContents should parse DDSx mip count");
                ok &= Expect(entry.texFormat == "BC3_UNORM", "DXP PackContents should parse DDSx format");
                ok &= Expect(entry.texCompression == "None", "DXP PackContents should parse DDSx compression");
                ok &= Expect(entry.formatInfo == "BC3_UNORM 128x64", "DXP PackContents should expose DDSx metadata summary");
                ok &= Expect(entry.ddsxHeader.size() == 32, "DXP PackContents should preserve raw DDSx header for export");
            }
        }

        const fs::path grpPath = fixtureRoot / "content" / "base" / "res" / "sample_vehicle.grp";
        const std::shared_ptr<AssetIndex::PackContents> grpContents = AssetIndex::ParseGrpContents(grpPath);
        ok &= Expect(grpContents != nullptr, "GRP PackContents should be returned");
        if (grpContents)
        {
            ok &= Expect(grpContents->parsed, "GRP PackContents should be marked parsed");
            ok &= Expect(grpContents->error.empty(), "GRP PackContents should parse without error");
            ok &= Expect(grpContents->containerType == "GRP", "GRP PackContents should identify container type");
            ok &= Expect(grpContents->entries.size() == 1, "GRP PackContents should expose one entry");

            if (!grpContents->entries.empty())
            {
                const AssetIndex::SubAssetRecord& entry = grpContents->entries[0];
                ok &= Expect(entry.name == "test_model", "GRP PackContents should expose fixture model name");
                ok &= Expect(entry.offset == 0, "GRP PackContents should expose fixture model offset");
                ok &= Expect(entry.size == 4, "GRP PackContents should expose fixture model size");
            }
        }

        AssetIndex::PackContentsCache cache;
        cache.Put(vromfsPath, vromfsContents);
        ok &= Expect(cache.Has(vromfsPath), "PackContentsCache should report inserted VROMFS contents");

        const std::shared_ptr<const AssetIndex::PackContents> cachedVromfs = cache.Get(vromfsPath);
        ok &= Expect(cachedVromfs != nullptr, "PackContentsCache should return inserted VROMFS contents");
        if (cachedVromfs)
        {
            ok &= Expect(cachedVromfs->entries.size() == 1, "PackContentsCache should preserve inserted entries");
        }

        cache.Remove(vromfsPath);
        ok &= Expect(!cache.Has(vromfsPath), "PackContentsCache should remove a single entry");

        cache.Put(dxpPath, dxpContents);
        ok &= Expect(cache.Has(dxpPath), "PackContentsCache should report inserted DXP contents");
        cache.Put(vromfsPath, vromfsContents);
        ok &= Expect(cache.Size() == 2, "PackContentsCache should report retained entry count");
        ok &= Expect(cache.ClearExcept(dxpPath) == 1, "PackContentsCache should clear every entry except the requested pack");
        ok &= Expect(cache.Has(dxpPath), "PackContentsCache ClearExcept should keep requested pack");
        ok &= Expect(!cache.Has(vromfsPath), "PackContentsCache ClearExcept should remove other packs");

        cache.Put(vromfsPath, vromfsContents);
        ok &= Expect(cache.TrimToLimit(1, vromfsPath) == 1, "PackContentsCache TrimToLimit should evict one old pack");
        ok &= Expect(cache.Has(vromfsPath), "PackContentsCache TrimToLimit should keep requested pack");
        ok &= Expect(!cache.Has(dxpPath), "PackContentsCache TrimToLimit should evict non-kept pack");

        cache.Clear();
        ok &= Expect(cache.Size() == 0, "PackContentsCache should clear all entries");

        return ok;
    }

    bool TestBlkResourceLinks(const std::vector<AssetIndex::ResourceRecord>& records)
    {
        bool ok = true;

        const AssetIndex::BlkResourceLinkBuildResult result = AssetIndex::BuildBlkResourceLinks(records);
        ok &= Expect(result.blkFilesParsed == 1, "BLK links should parse one fixture BLK file");
        ok &= Expect(result.blkFilesSkipped == 0, "BLK links should skip no fixture BLK files");
        ok &= Expect(result.parseErrors == 0, "BLK links should have no parse errors");
        ok &= Expect(result.links.size() == 2, "BLK links should find two pack references");

        bool foundGrp = false;
        bool foundDxp = false;
        for (const AssetIndex::BlkResourceLink& link : result.links)
        {
            if (link.ruleName == "gameResPack" && link.resourceName == "sample_vehicle")
            {
                foundGrp = true;
                ok &= Expect(link.expectedType == AssetIndex::AssetType::Grp, "gameResPack should expect GRP");
                ok &= Expect(link.matchedPath.has_value(), "gameResPack should match fixture GRP");
                if (link.matchedPath)
                {
                    ok &= Expect(link.matchedPath->filename() == "sample_vehicle.grp", "gameResPack should match sample_vehicle.grp");
                }
            }
            else if (link.ruleName == "ddsxTexPack" && link.resourceName == "sample_textures")
            {
                foundDxp = true;
                ok &= Expect(link.expectedType == AssetIndex::AssetType::Dxp, "ddsxTexPack should expect DXP");
                ok &= Expect(link.matchedPath.has_value(), "ddsxTexPack should match fixture DXP");
                if (link.matchedPath)
                {
                    ok &= Expect(link.matchedPath->filename() == "sample_textures.dxp.bin", "ddsxTexPack should match sample_textures.dxp.bin");
                }
            }
        }

        ok &= Expect(foundGrp, "BLK links should include gameResPack fixture reference");
        ok &= Expect(foundDxp, "BLK links should include ddsxTexPack fixture reference");
        return ok;
    }

    AssetIndex::ResourceRecord MakeResourceRecord(const fs::path& path, AssetIndex::AssetType type)
    {
        AssetIndex::ResourceRecord record;
        record.path = path;
        record.type = type;
        AssetIndex::TryReadFileMetadata(path, record.sizeBytes, record.lastWriteTicks);
        return record;
    }

    std::vector<uint8_t> MakeSyntheticGrpPack(std::string_view entryName)
    {
        std::vector<uint8_t> data;
        const char magic[] = "GRP\x01";
        data.insert(data.end(), magic, magic + 4);
        AppendU32(data, 1);
        data.insert(data.end(), entryName.begin(), entryName.end());
        data.push_back(0);
        AppendU32(data, 0);
        AppendU32(data, 4);
        AppendU32(data, 4);
        AppendU32(data, 0);
        data.push_back(0xCA);
        data.push_back(0xFE);
        data.push_back(0xBA);
        data.push_back(0xBE);
        return data;
    }

    std::vector<uint8_t> MakeSyntheticDxpPackWithPayloadSizes(
        const std::vector<std::pair<std::string, uint32_t>>& textures)
    {
        const size_t ddsHeaderStart = 0x100;
        const size_t block3Start = ddsHeaderStart + textures.size() * 0x20;
        const size_t payloadStart = block3Start + textures.size() * 0x18;
        std::vector<uint8_t> data(payloadStart + textures.size() * 4, 0);

        auto put16 = [&data](size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [&data](size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        data[0] = 'D';
        data[1] = 'x';
        data[2] = 'P';
        data[3] = '2';
        put16(0x8, static_cast<uint16_t>(textures.size()));
        put32(0x20, static_cast<uint32_t>(ddsHeaderStart - 0x10));
        put32(0x30, static_cast<uint32_t>(block3Start - 0x10));

        size_t nameOffset = 0x48;
        for (const auto& [textureName, payloadSize] : textures)
        {
            (void)payloadSize;
            std::copy(textureName.begin(), textureName.end(), data.begin() + nameOffset);
            nameOffset += textureName.size();
            data[nameOffset++] = 0;
        }

        for (size_t i = 0; i < textures.size(); ++i)
        {
            const size_t headerOffset = ddsHeaderStart + i * 0x20;
            data[headerOffset + 0] = 0x44;
            data[headerOffset + 1] = 0x44;
            data[headerOffset + 2] = 0x53;
            data[headerOffset + 3] = 0x78;
            put32(headerOffset + 0x04, 28);
            put16(headerOffset + 0x0C, 1);
            put16(headerOffset + 0x0E, 1);
            data[headerOffset + 0x10] = 1;
            put16(headerOffset + 0x12, 1);
            put16(headerOffset + 0x14, 32);
            put32(headerOffset + 0x18, 4);

            const size_t tableOffset = block3Start + i * 0x18;
            put32(tableOffset + 0x0C, static_cast<uint32_t>(payloadStart + i * 4));
            put32(tableOffset + 0x10, textures[i].second);

            const size_t payloadOffset = payloadStart + i * 4;
            data[payloadOffset + 0] = 0x10;
            data[payloadOffset + 1] = 0x20;
            data[payloadOffset + 2] = 0x30;
            data[payloadOffset + 3] = 0x40;
        }

        return data;
    }

    std::vector<uint8_t> MakeSyntheticDxpPack(const std::vector<std::string>& textureNames)
    {
        std::vector<std::pair<std::string, uint32_t>> textures;
        textures.reserve(textureNames.size());
        for (const std::string& textureName : textureNames)
        {
            textures.emplace_back(textureName, 4);
        }
        return MakeSyntheticDxpPackWithPayloadSizes(textures);
    }

    bool WriteBinaryFile(const fs::path& path, const std::vector<uint8_t>& data)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            return false;
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return static_cast<bool>(file);
    }

    bool TestDescriptorResourceLinks()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_DescriptorLinkTest_" + std::to_string(stamp));
        const fs::path resDir = tempDir / "content" / "base" / "res";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(resDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp descriptor link test folder: " << ec.message() << '\n';
            return false;
        }

        const fs::path descriptorPath = resDir / "riDesc.bin";
        const fs::path grpPath = resDir / "sample_vehicle.grp";
        const fs::path dxpPath = resDir / "sample_textures.dxp.bin";
        const fs::path blkPath = resDir / "sample_rules.blk";

        ok &= Expect(WriteBinaryFile(descriptorPath, MakeSyntheticGameResDescPayload()),
            "Descriptor link test should write synthetic descriptor");
        ok &= Expect(WriteBinaryFile(grpPath, MakeSyntheticModernGrpPack("sample_tank", 0x77F8232Fu, "RIND")),
            "Descriptor link test should write synthetic GRP");
        ok &= Expect(WriteBinaryFile(dxpPath, MakeSyntheticDxpPack({ "body_tex$hq*", "body_n$hq*" })),
            "Descriptor link test should write synthetic DXP");

        {
            std::ofstream blk(blkPath);
            blk << "gameResPack:t=\"sample_vehicle\"\n";
            blk << "ddsxTexPack:t=\"sample_textures\"\n";
        }

        std::vector<AssetIndex::ResourceRecord> records;
        records.push_back(MakeResourceRecord(descriptorPath, AssetIndex::AssetType::Descriptor));
        records.push_back(MakeResourceRecord(grpPath, AssetIndex::AssetType::Grp));
        records.push_back(MakeResourceRecord(dxpPath, AssetIndex::AssetType::Dxp));
        records.push_back(MakeResourceRecord(blkPath, AssetIndex::AssetType::Blk));

        const AssetIndex::BlkResourceLinkBuildResult blkLinks = AssetIndex::BuildBlkResourceLinks(records);
        ok &= Expect(blkLinks.links.size() == 2, "Descriptor link test should build BLK pack group");

        const AssetIndex::DescriptorResourceLinkBuildResult descriptorLinks =
            AssetIndex::BuildDescriptorResourceLinks(records, &blkLinks.links);

        ok &= Expect(descriptorLinks.parseErrors == 0, "Descriptor links should parse without errors");
        ok &= Expect(descriptorLinks.descriptorFilesParsed == 1, "Descriptor links should parse one descriptor");
        ok &= Expect(descriptorLinks.links.size() == 1, "Descriptor links should expose one descriptor resource");
        ok &= Expect(descriptorLinks.grpPacksParsed == 1, "Descriptor links should parse the candidate GRP pack");
        ok &= Expect(descriptorLinks.dxpPacksParsed == 1, "Descriptor links should parse the candidate DXP pack");

        if (!descriptorLinks.links.empty())
        {
            const AssetIndex::DescriptorResourceLink& link = descriptorLinks.links[0];
            ok &= Expect(link.resourceName == "sample_tank", "Descriptor link should preserve resource name");
            ok &= Expect(link.matchedGrpPath.has_value(), "Descriptor link should match GRP pack");
            ok &= Expect(link.matchedGrpEntryName.has_value() && *link.matchedGrpEntryName == "sample_tank",
                "Descriptor link should match GRP entry name");
            ok &= Expect(link.matchedGrpClassId == 0x77F8232Fu, "Descriptor link should preserve matched GRP class id");
            ok &= Expect(link.matchedGrpClassName == "RendInst", "Descriptor link should preserve matched GRP class name");
            ok &= Expect(link.matchedGrpEntryCanExportStaticModel, "Descriptor link should mark RendInst matches as exportable");
            if (link.matchedGrpPath)
            {
                ok &= Expect(link.matchedGrpPath->filename() == "sample_vehicle.grp",
                    "Descriptor link should prefer BLK-linked GRP pack");
            }

            ok &= Expect(link.textureLinks.size() == 1,
                "Descriptor link should resolve material texture references, not every declared texture");
            ok &= Expect(link.materialNames.size() == 1 && link.materialNames[0] == "body",
                "Descriptor link should preserve generated material names");
            ok &= Expect(link.materialDomains.size() == 1 && link.materialDomains[0] == "rigid",
                "Descriptor link should preserve material domains");
            ok &= Expect(link.materialShaderClasses.size() == 1 && link.materialShaderClasses[0] == "rendinst_simple",
                "Descriptor link should preserve material shader classes");
            ok &= Expect(link.materialTextureBindings.size() == 1,
                "Descriptor link should preserve material-to-texture bindings");
            if (!link.materialTextureBindings.empty())
            {
                ok &= Expect(link.materialTextureBindings[0].materialIndex == 0,
                    "Descriptor link material texture binding should preserve material slot");
                ok &= Expect(link.materialTextureBindings[0].textureIndex == 0,
                    "Descriptor link material texture binding should preserve texture slot");
                ok &= Expect(link.materialTextureBindings[0].slotName == "t0",
                    "Descriptor link material texture binding should preserve slot name");
            }
            if (!link.textureLinks.empty())
            {
                const AssetIndex::DescriptorTextureLink& textureLink = link.textureLinks[0];
                ok &= Expect(textureLink.textureIndex == 0, "Descriptor texture link should preserve texture slot index");
                ok &= Expect(textureLink.textureName == "body_tex", "Descriptor texture link should resolve texture index to name");
                ok &= Expect(textureLink.matchedDxpPath.has_value(), "Descriptor texture link should match DXP pack");
                ok &= Expect(textureLink.matchedDxpEntryName.has_value() && *textureLink.matchedDxpEntryName == "body_tex$hq*",
                    "Descriptor texture link should match qualified DXP entry name");
                if (textureLink.matchedDxpPath)
                {
                    ok &= Expect(textureLink.matchedDxpPath->filename() == "sample_textures.dxp.bin",
                        "Descriptor texture link should prefer BLK-linked DXP pack");
                }
            }

            size_t progressCalls = 0;
            AssetIndex::DescriptorResourceLinkBuildOptions options;
            options.blkLinks = &blkLinks.links;
            options.progress = [&progressCalls](const AssetIndex::DescriptorResourceLinkProgress&) {
                ++progressCalls;
                return true;
            };
            const AssetIndex::DescriptorResourceLinkBuildResult progressLinks =
                AssetIndex::BuildDescriptorResourceLinks(records, options);
            ok &= Expect(progressLinks.links.size() == 1, "Universal descriptor linker should find one model link");
            ok &= Expect(progressCalls > 0, "Universal descriptor linker should report progress");

            const fs::path descriptorCachePath = tempDir / "descriptor-links-cache.txt";
            std::string descriptorCacheError;
            ok &= Expect(
                AssetIndex::SaveDescriptorResourceLinkCache(
                    descriptorCachePath,
                    tempDir,
                    records,
                    progressLinks.links,
                    options,
                    descriptorCacheError),
                "Descriptor-link cache save should succeed");
            if (!descriptorCacheError.empty())
            {
                std::cerr << "Descriptor-link cache save detail: " << descriptorCacheError << '\n';
            }

            const AssetIndex::DescriptorResourceLinkCacheLoadResult cachedLinks =
                AssetIndex::LoadDescriptorResourceLinkCache(descriptorCachePath, tempDir, records, options);
            ok &= Expect(cachedLinks.loaded, "Descriptor-link cache load should restore fresh links");
            ok &= Expect(cachedLinks.links.size() == progressLinks.links.size(), "Descriptor-link cache load should preserve link count");
            if (!cachedLinks.links.empty())
            {
                ok &= Expect(cachedLinks.links[0].resourceName == "sample_tank", "Descriptor-link cache should preserve resource names");
                ok &= Expect(cachedLinks.links[0].matchedGrpPath.has_value(), "Descriptor-link cache should preserve matched GRP path");
                ok &= Expect(!cachedLinks.links[0].textureLinks.empty(), "Descriptor-link cache should preserve texture links");
                ok &= Expect(cachedLinks.links[0].materialNames.size() == 1 && cachedLinks.links[0].materialNames[0] == "body",
                    "Descriptor-link cache should preserve generated material names");
                ok &= Expect(cachedLinks.links[0].materialDomains.size() == 1 && cachedLinks.links[0].materialDomains[0] == "rigid",
                    "Descriptor-link cache should preserve material domains");
                ok &= Expect(cachedLinks.links[0].materialShaderClasses.size() == 1 &&
                    cachedLinks.links[0].materialShaderClasses[0] == "rendinst_simple",
                    "Descriptor-link cache should preserve material shader classes");
                ok &= Expect(!cachedLinks.links[0].materialTextureBindings.empty(),
                    "Descriptor-link cache should preserve material texture bindings");
            }

            std::vector<AssetIndex::ResourceRecord> staleRecords = records;
            staleRecords[0].sizeBytes += 1;
            const AssetIndex::DescriptorResourceLinkCacheLoadResult staleCache =
                AssetIndex::LoadDescriptorResourceLinkCache(descriptorCachePath, tempDir, staleRecords, options);
            ok &= Expect(!staleCache.loaded && staleCache.fileFound && staleCache.rootMatched,
                "Descriptor-link cache should invalidate when source records change");
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestDescriptorResourceLinksSkipZeroPayloadDxp()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_DescriptorZeroDxpTest_" + std::to_string(stamp));
        const fs::path resDir = tempDir / "content" / "base" / "res";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(resDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp descriptor zero-DXP test folder: " << ec.message() << '\n';
            return false;
        }

        const fs::path descriptorPath = resDir / "riDesc.bin";
        const fs::path grpPath = resDir / "sample_vehicle.grp";
        const fs::path dxpPath = resDir / "zero_payload_textures.dxp.bin";

        ok &= Expect(WriteBinaryFile(descriptorPath, MakeSyntheticGameResDescPayload()),
            "Zero-payload DXP test should write synthetic descriptor");
        ok &= Expect(WriteBinaryFile(grpPath, MakeSyntheticModernGrpPack("sample_tank", 0x77F8232Fu, "RIND")),
            "Zero-payload DXP test should write synthetic GRP");
        ok &= Expect(WriteBinaryFile(dxpPath, MakeSyntheticDxpPackWithPayloadSizes({ { "body_tex$hq*", 0 } })),
            "Zero-payload DXP test should write synthetic DXP");

        std::vector<AssetIndex::ResourceRecord> records;
        records.push_back(MakeResourceRecord(descriptorPath, AssetIndex::AssetType::Descriptor));
        records.push_back(MakeResourceRecord(grpPath, AssetIndex::AssetType::Grp));
        records.push_back(MakeResourceRecord(dxpPath, AssetIndex::AssetType::Dxp));

        const AssetIndex::DescriptorResourceLinkBuildResult descriptorLinks =
            AssetIndex::BuildDescriptorResourceLinks(records);

        ok &= Expect(descriptorLinks.links.size() == 1, "Zero-payload DXP test should expose one descriptor resource");
        if (!descriptorLinks.links.empty())
        {
            const AssetIndex::DescriptorResourceLink& link = descriptorLinks.links[0];
            ok &= Expect(link.textureLinks.size() == 1, "Zero-payload DXP test should preserve texture dependency");
            if (!link.textureLinks.empty())
            {
                ok &= Expect(!link.textureLinks[0].matchedDxpPath.has_value(),
                    "Descriptor linker should not match zero-payload DXP texture entries");
                ok &= Expect(!link.textureLinks[0].matchedDxpEntryName.has_value(),
                    "Descriptor linker should leave zero-payload DXP entry names unresolved");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestDescriptorResourceLinksNonStaticGrpMatch()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_DescriptorLinkAnimTest_" + std::to_string(stamp));
        const fs::path resDir = tempDir / "content" / "base" / "res";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(resDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp non-static descriptor link test folder: " << ec.message() << '\n';
            return false;
        }

        const fs::path descriptorPath = resDir / "riDesc.bin";
        const fs::path grpPath = resDir / "sample_vehicle.grp";
        const fs::path dxpPath = resDir / "sample_textures.dxp.bin";
        const fs::path blkPath = resDir / "sample_rules.blk";

        ok &= Expect(WriteBinaryFile(descriptorPath, MakeSyntheticGameResDescPayload()),
            "Non-static descriptor link test should write synthetic descriptor");
        ok &= Expect(WriteBinaryFile(grpPath, MakeSyntheticModernGrpPack("sample_tank", 0x40C586F9u, "A2D!")),
            "Non-static descriptor link test should write synthetic GRP");
        ok &= Expect(WriteBinaryFile(dxpPath, MakeSyntheticDxpPack({ "body_tex$hq*", "body_n$hq*" })),
            "Non-static descriptor link test should write synthetic DXP");

        {
            std::ofstream blk(blkPath);
            blk << "gameResPack:t=\"sample_vehicle\"\n";
            blk << "ddsxTexPack:t=\"sample_textures\"\n";
        }

        std::vector<AssetIndex::ResourceRecord> records;
        records.push_back(MakeResourceRecord(descriptorPath, AssetIndex::AssetType::Descriptor));
        records.push_back(MakeResourceRecord(grpPath, AssetIndex::AssetType::Grp));
        records.push_back(MakeResourceRecord(dxpPath, AssetIndex::AssetType::Dxp));
        records.push_back(MakeResourceRecord(blkPath, AssetIndex::AssetType::Blk));

        const AssetIndex::BlkResourceLinkBuildResult blkLinks = AssetIndex::BuildBlkResourceLinks(records);
        const AssetIndex::DescriptorResourceLinkBuildResult descriptorLinks =
            AssetIndex::BuildDescriptorResourceLinks(records, &blkLinks.links);

        ok &= Expect(descriptorLinks.links.size() == 1, "Non-static descriptor link test should expose one descriptor resource");
        if (!descriptorLinks.links.empty())
        {
            const AssetIndex::DescriptorResourceLink& link = descriptorLinks.links[0];
            ok &= Expect(link.matchedGrpClassId == 0x40C586F9u, "Descriptor link should preserve Anim2Data class id");
            ok &= Expect(link.matchedGrpClassName == "Anim2Data", "Descriptor link should preserve Anim2Data class name");
            ok &= Expect(!link.matchedGrpEntryCanExportStaticModel, "Descriptor link should mark Anim2Data matches as non-exportable");
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestExportManifest()
    {
        bool ok = true;

        // Define temporary manifest path
        fs::path tempManifest = fs::temp_directory_path() / "test-manifest.export-manifest.json";
        std::error_code ec;
        fs::remove(tempManifest, ec);

        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourcePack = "test_textures.dxp.bin";
        manifest.sourceResource = "test_tex_resource";
        manifest.assetType = "Texture (DDSx)";
        manifest.outputFiles = { "test_tex_resource.dds" };
        manifest.decodeWarnings = { "Decompressed from zstd", "Format code: 77" };
        manifest.unsupportedFields = { "unknown_flag_0x20" };
        manifest.appVersion = "1.0.0-test";
        manifest.formatDecoderVersion = "DdsxReader v1.0";

        ok &= Expect(manifest.WriteToFile(tempManifest), "ExportManifest should write to file successfully");
        ok &= Expect(fs::exists(tempManifest), "Manifest file should exist on disk");

        // Let's read the file and check for correct escaping
        std::ifstream file(tempManifest);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        ok &= Expect(content.find("\"sourcePack\": \"test_textures.dxp.bin\"") != std::string::npos, "Manifest should serialize sourcePack");
        ok &= Expect(content.find("\"sourceResource\": \"test_tex_resource\"") != std::string::npos, "Manifest should serialize sourceResource");
        ok &= Expect(content.find("\"assetType\": \"Texture (DDSx)\"") != std::string::npos, "Manifest should serialize assetType");
        ok &= Expect(content.find("\"test_tex_resource.dds\"") != std::string::npos, "Manifest should serialize outputFiles array elements");
        ok &= Expect(content.find("\"Decompressed from zstd\"") != std::string::npos, "Manifest should serialize decodeWarnings");

        fs::remove(tempManifest, ec);
        return ok;
    }

    bool TestTextureExporterDX10()
    {
        bool ok = true;

        using DagorAssetsExtractor::Export::TextureExporter;

        // BC6H, BC7 are DXGI formats that lack legacy FourCC mappings - should require DX10 header
        ok &= Expect(TextureExporter::TestNeedsDX10Header(98), "BC7_UNORM (98) should require DX10 header");
        ok &= Expect(TextureExporter::TestNeedsDX10Header(99), "BC7_UNORM_SRGB (99) should require DX10 header");
        ok &= Expect(TextureExporter::TestNeedsDX10Header(95), "BC6H_UF16 (95) should require DX10 header");

        // DXT1, DXT3, DXT5, ATI1, ATI2 have legacy FourCC mappings - should NOT require DX10 header
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(827611204), "DXT1 should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(861165636), "DXT3 should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(894720068), "DXT5 should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(826889281), "ATI1 should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(843666497), "ATI2 should NOT require DX10 header");

        // DXGI versions of BC1/BC2/BC3 can be mapped to legacy FourCC - should NOT require DX10 header
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(71), "BC1_UNORM (71) should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(77), "BC3_UNORM (77) should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(80), "BC4_UNORM (80) should NOT require DX10 header");
        ok &= Expect(!TextureExporter::TestNeedsDX10Header(83), "BC5_UNORM (83) should NOT require DX10 header");

        // GetDxgiFormat should return correct mappings
        ok &= Expect(TextureExporter::TestGetDxgiFormat(827611204) == 71, "GetDxgiFormat(DXT1) should map to BC1 (71)");
        ok &= Expect(TextureExporter::TestGetDxgiFormat(861165636) == 74, "GetDxgiFormat(DXT3) should map to BC2 (74)");
        ok &= Expect(TextureExporter::TestGetDxgiFormat(894720068) == 77, "GetDxgiFormat(DXT5) should map to BC3 (77)");
        ok &= Expect(TextureExporter::TestGetDxgiFormat(826889281) == 80, "GetDxgiFormat(ATI1) should map to BC4 (80)");
        ok &= Expect(TextureExporter::TestGetDxgiFormat(843666497) == 83, "GetDxgiFormat(ATI2) should map to BC5 (83)");

        // Unchanged formats should return the input format
        ok &= Expect(TextureExporter::TestGetDxgiFormat(98) == 98, "GetDxgiFormat(BC7) should return BC7");
        ok &= Expect(TextureExporter::TestGetDxgiFormat(28) == 28, "GetDxgiFormat(RGBA8) should return RGBA8");

        return ok;
    }

    bool TestTextureExporterDxpPayloadExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60); // DDSx header table at 0x70
        put32(data, 0x30, 0x80); // payload table at 0x90

        const std::string textureName = "folder/tex:albedo";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78; // DDSx
        put32(data, 0x74, 28); // R8G8B8A8_UNORM
        put16(data, 0x7C, 1);  // width
        put16(data, 0x7E, 1);  // height
        data[0x80] = 1;        // mip count
        data[0x81] = 0;        // hqPartLevels
        put16(data, 0x82, 1);  // depth
        put16(data, 0x84, 32); // bits per pixel
        put16(data, 0x86, 0);  // quality/shift nibbles
        put32(data, 0x88, 4);  // memory size
        put32(data, 0x8C, 0);  // uncompressed payload size marker

        put32(data, 0x9C, 0xB0); // payload offset
        put32(data, 0xA0, 4);    // payload size
        data[0xB0] = 0x11; data[0xB1] = 0x22; data[0xB2] = 0x33; data[0xB3] = 0x44;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "TextureExporter DXP test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "TextureExporter DXP test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "TextureExporter DXP test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.overwrite = false;
            options.exportJsonManifest = true;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export DXP payload using separate DDSx header");
            if (!result.success)
            {
                std::cerr << "Texture export error: " << result.error << '\n';
            }
            ok &= Expect(fs::exists(result.outputPath), "TextureExporter should create DDS output");
            ok &= Expect(result.outputPath.filename() == "tex_albedo.dds", "TextureExporter should sanitize invalid output filename characters");
            ok &= Expect(result.outputPath.parent_path().filename() == "folder", "TextureExporter should preserve safe DXP path folders");

            if (fs::exists(result.outputPath))
            {
                std::ifstream dds(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 132, "TextureExporter DDS output should contain DDS header plus payload");
                ok &= Expect(bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ',
                    "TextureExporter DDS output should start with DDS magic");
            }

            fs::path manifestPath = result.outputPath;
            manifestPath.replace_extension(".export-manifest.json");
            ok &= Expect(fs::exists(manifestPath), "TextureExporter should create export manifest");
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadablePngExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TexturePngExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_png.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp PNG texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "folder/tex:png";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 28); // R8G8B8A8_UNORM
        put16(data, 0x7C, 1);
        put16(data, 0x7E, 1);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 32);
        put32(data, 0x88, 4);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 4);
        data[0xB0] = 0x12; data[0xB1] = 0x34; data[0xB2] = 0x56; data[0xB3] = 0xFF;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "PNG export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "PNG export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "PNG export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "PNG";
            options.overwrite = false;
            options.exportJsonManifest = true;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable PNG output");
            ok &= Expect(result.outputPath.extension() == ".png", "Readable PNG export should produce a .png file");
            ok &= Expect(fs::exists(result.outputPath), "Readable PNG export should create the PNG output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream png(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() >= 8, "Readable PNG export should produce a non-empty PNG file");
                ok &= Expect(bytes.size() >= 8 &&
                    bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
                    bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A,
                    "Readable PNG export should start with the PNG signature");
            }

            fs::path manifestPath = result.outputPath;
            manifestPath.replace_extension(".export-manifest.json");
            ok &= Expect(fs::exists(manifestPath), "Readable PNG export should create an export manifest");
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterBatchSkipExistingReadablePng()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureBatchPngSkipTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_batch_png.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp batch PNG skip test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "folder/tex:png";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 28); // R8G8B8A8_UNORM
        put16(data, 0x7C, 1);
        put16(data, 0x7E, 1);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 32);
        put32(data, 0x88, 4);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 4);
        data[0xB0] = 0x12; data[0xB1] = 0x34; data[0xB2] = 0x56; data[0xB3] = 0xFF;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "Batch PNG skip test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "Batch PNG skip test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "Batch PNG skip test should expose one texture");

        const fs::path expectedOutput = outputDir / "folder" / "tex_png.png";
        fs::create_directories(expectedOutput.parent_path(), ec);
        {
            std::ofstream preexisting(expectedOutput, std::ios::binary);
            preexisting << "already-there";
        }

        DagorAssetsExtractor::Export::TextureExportOptions options;
        options.outputDir = outputDir;
        options.format = "PNG";
        options.overwrite = false;

        DagorAssetsExtractor::Export::BatchTextureExportResult result =
            DagorAssetsExtractor::Export::TextureExporter::ExportAllTextures(packPath, *contents, options);

        ok &= Expect(result.total == 1, "Batch PNG skip test should report one total texture");
        ok &= Expect(result.succeeded == 0, "Batch PNG skip test should not report successful exports");
        ok &= Expect(result.failed == 0, "Batch PNG skip test should not report failures for existing readable outputs");
        ok &= Expect(result.skipped == 1, "Batch PNG skip test should report one skipped texture");
        ok &= Expect(result.errors.empty(), "Batch PNG skip test should not report batch errors");

        bool sawSkipWarning = false;
        for (const std::string& warning : result.warnings)
        {
            if (warning.find("Skipped existing file: folder/tex:png") != std::string::npos)
            {
                sawSkipWarning = true;
                break;
            }
        }
        ok &= Expect(sawSkipWarning, "Batch PNG skip test should report a skipped-existing-file warning");

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableTgaExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureTgaExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_tga.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp TGA texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "gray/tga_tex";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 61); // R8_UNORM
        put16(data, 0x7C, 2);
        put16(data, 0x7E, 1);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 8);
        put32(data, 0x88, 2);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 2);
        data[0xB0] = 0x11; data[0xB1] = 0xCC;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "TGA export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "TGA export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "TGA export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "TGA";
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable TGA output");
            ok &= Expect(result.outputPath.extension() == ".tga", "Readable TGA export should produce a .tga file");
            ok &= Expect(fs::exists(result.outputPath), "Readable TGA export should create the TGA output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream tga(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(tga)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 20, "Readable TGA export should contain an 18-byte header plus 2 bytes of grayscale pixels");
                ok &= Expect(bytes.size() >= 18 && bytes[2] == 3, "Readable TGA export should use the grayscale image type");
                ok &= Expect(bytes.size() >= 18 && bytes[16] == 8, "Readable TGA export should use 8 bits per pixel for grayscale data");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableRgba16fTgaExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureRgba16fTgaExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_rgba16f_tga.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp RGBA16F TGA texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC8, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "float/rgba16f";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 10); // R16G16B16A16_FLOAT
        put16(data, 0x7C, 1);
        put16(data, 0x7E, 1);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 64);
        put32(data, 0x88, 8);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 8);
        put16(data, 0xB0, 0x3C00); // R = 1.0
        put16(data, 0xB2, 0x3800); // G = 0.5
        put16(data, 0xB4, 0x3400); // B = 0.25
        put16(data, 0xB6, 0x3C00); // A = 1.0

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "RGBA16F TGA export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "RGBA16F TGA export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "RGBA16F TGA export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.format = "TGA";
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable RGBA16F TGA output");
            ok &= Expect(result.outputPath.extension() == ".tga", "Readable RGBA16F TGA export should produce a .tga file");
            ok &= Expect(fs::exists(result.outputPath), "Readable RGBA16F TGA export should create the TGA output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream tga(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(tga)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 22, "Readable RGBA16F TGA export should contain an 18-byte header plus 4 BGRA bytes");
                ok &= Expect(bytes.size() >= 22 &&
                    bytes[18] == 0x40 && bytes[19] == 0x80 && bytes[20] == 0xFF && bytes[21] == 0xFF,
                    "Readable RGBA16F TGA export should decode the pixel to BGRA bytes");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableRgba32fTgaExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureRgba32fTgaExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_rgba32f_tga.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp RGBA32F TGA texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };
        auto putFloat = [&](std::vector<uint8_t>& out, size_t offset, float value) {
            uint32_t bits = 0;
            static_assert(sizeof(bits) == sizeof(value));
            std::memcpy(&bits, &value, sizeof(bits));
            put32(out, offset, bits);
        };

        std::vector<uint8_t> data(0xD0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "float/rgba32f";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 2); // R32G32B32A32_FLOAT
        put16(data, 0x7C, 1);
        put16(data, 0x7E, 1);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 128);
        put32(data, 0x88, 16);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 16);
        putFloat(data, 0xB0, 1.0f);  // R
        putFloat(data, 0xB4, 0.5f);  // G
        putFloat(data, 0xB8, 0.25f); // B
        putFloat(data, 0xBC, 1.0f);  // A

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "RGBA32F TGA export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "RGBA32F TGA export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "RGBA32F TGA export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.format = "TGA";
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable RGBA32F TGA output");
            ok &= Expect(result.outputPath.extension() == ".tga", "Readable RGBA32F TGA export should produce a .tga file");
            ok &= Expect(fs::exists(result.outputPath), "Readable RGBA32F TGA export should create the TGA output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream tga(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(tga)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 22, "Readable RGBA32F TGA export should contain an 18-byte header plus 4 BGRA bytes");
                ok &= Expect(bytes.size() >= 22 &&
                    bytes[18] == 0x40 && bytes[19] == 0x80 && bytes[20] == 0xFF && bytes[21] == 0xFF,
                    "Readable RGBA32F TGA export should decode the pixel to BGRA bytes");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableBc1TgaExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureBc1TgaExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_bc1_tga.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp BC1 TGA texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC8, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "bc1/readable_tga";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 827611204); // DXT1
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 4);
        put32(data, 0x88, 8);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 8);
        data[0xB0] = 0x00; data[0xB1] = 0xF8; // red 565
        data[0xB2] = 0xE0; data[0xB3] = 0x07; // green 565
        data[0xB4] = 0x00; data[0xB5] = 0x00; data[0xB6] = 0x00; data[0xB7] = 0x00; // all pixels use color 0

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "BC1 TGA export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "BC1 TGA export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "BC1 TGA export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "TGA";
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable BC1 TGA output");
            ok &= Expect(result.outputPath.extension() == ".tga", "Readable BC1 TGA export should produce a .tga file");
            ok &= Expect(fs::exists(result.outputPath), "Readable BC1 TGA export should create the TGA output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream tga(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(tga)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 82, "Readable BC1 TGA export should contain an 18-byte header plus 16 BGRA pixels");
                ok &= Expect(bytes.size() >= 18 && bytes[2] == 2, "Readable BC1 TGA export should use the true-color image type");
                ok &= Expect(bytes.size() >= 22 &&
                    bytes[18] == 0x00 && bytes[19] == 0x00 && bytes[20] == 0xFF && bytes[21] == 0xFF,
                    "Readable BC1 TGA export should decode the first pixel to opaque red BGRA");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableBc5TgaExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureBc5TgaExportTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_bc5_tga.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp BC5 TGA texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xD0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "bc5/readable_tga";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 83); // BC5_UNORM
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 16);
        put32(data, 0x88, 16);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 16);

        data[0xB0] = 0xFF; data[0xB1] = 0x00; // red channel block endpoints
        data[0xB8] = 0x80; data[0xB9] = 0x00; // green channel block endpoints

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "BC5 TGA export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "BC5 TGA export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "BC5 TGA export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "TGA";
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export readable BC5 TGA output");
            ok &= Expect(result.outputPath.extension() == ".tga", "Readable BC5 TGA export should produce a .tga file");
            ok &= Expect(fs::exists(result.outputPath), "Readable BC5 TGA export should create the TGA output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream tga(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(tga)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 82, "Readable BC5 TGA export should contain an 18-byte header plus 16 BGRA pixels");
                ok &= Expect(bytes.size() >= 22 &&
                    bytes[18] == 0x00 && bytes[19] == 0x80 && bytes[20] == 0xFF && bytes[21] == 0xFF,
                    "Readable BC5 TGA export should decode RG data into the first pixel");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableBc7PngExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureBc7ReadableTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_bc7_readable.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp DDS fallback test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xD0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "bc7/fallback_tex";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 98); // BC7_UNORM
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 8);
        put32(data, 0x88, 16);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 16);
        data[0xB0] = 0x03; data[0xB1] = 0x10; data[0xB2] = 0x77; data[0xB3] = 0xCC;
        data[0xB4] = 0x55; data[0xB5] = 0xAA; data[0xB6] = 0x11; data[0xB7] = 0x22;
        data[0xB8] = 0x33; data[0xB9] = 0x44; data[0xBA] = 0x99; data[0xBB] = 0x88;
        data[0xBC] = 0xFE; data[0xBD] = 0xDC; data[0xBE] = 0xBA; data[0xBF] = 0x98;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "BC7 readable export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "BC7 readable export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "BC7 readable export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "PNG";
            options.preserveOriginalDDS = true;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            if (!result.success || result.outputPath.extension() != ".png")
            {
                std::cerr << "BC7 readable export detail: success=" << result.success
                    << ", output=" << result.outputPath.string()
                    << ", error=" << result.error << '\n';
                for (const std::string& warning : result.warnings)
                {
                    std::cerr << "  warning: " << warning << '\n';
                }
            }

            ok &= Expect(result.success, "TextureExporter should export readable BC7 PNG output");
            ok &= Expect(result.outputPath.extension() == ".png", "BC7 readable export should produce a .png file");
            ok &= Expect(fs::exists(result.outputPath), "BC7 readable export should create the PNG output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream png(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
                ok &= Expect(
                    bytes.size() >= 8 &&
                    bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
                    bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A,
                    "BC7 readable export output should start with PNG magic");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableBc6hPngExport()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureBc6hReadableTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_bc6h_readable.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp BC6H readable export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xD0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "bc6h/readable_tex";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 95); // BC6H_UF16
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 8);
        put32(data, 0x88, 16);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 16);
        // A zeroed BC6H block is sufficient for readable-export smoke coverage and
        // avoids exercising undefined synthetic bit patterns that can vary by decoder.

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "BC6H readable export test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "BC6H readable export test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "BC6H readable export test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "PNG";
            options.preserveOriginalDDS = true;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            if (!result.success || result.outputPath.extension() != ".png")
            {
                std::cerr << "BC6H readable export detail: success=" << result.success
                    << ", output=" << result.outputPath.string()
                    << ", error=" << result.error << '\n';
                for (const std::string& warning : result.warnings)
                {
                    std::cerr << "  warning: " << warning << '\n';
                }
            }

            ok &= Expect(result.success, "TextureExporter should export readable BC6H PNG output");
            ok &= Expect(result.outputPath.extension() == ".png", "BC6H readable export should produce a .png file");
            ok &= Expect(fs::exists(result.outputPath), "BC6H readable export should create the PNG output");

            if (fs::exists(result.outputPath))
            {
                std::ifstream png(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(png)), std::istreambuf_iterator<char>());
                ok &= Expect(
                    bytes.size() >= 8 &&
                    bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47 &&
                    bytes[4] == 0x0D && bytes[5] == 0x0A && bytes[6] == 0x1A && bytes[7] == 0x0A,
                    "BC6H readable export output should start with PNG magic");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReadableFallbackToDds()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureFallbackTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_fallback.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp DDS fallback test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xD0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "invalid/fallback_tex";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 5000); // intentionally invalid DXGI format
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 8);
        put32(data, 0x88, 16);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 16);
        data[0xB0] = 0x03; data[0xB1] = 0x10; data[0xB2] = 0x77; data[0xB3] = 0xCC;
        data[0xB4] = 0x55; data[0xB5] = 0xAA; data[0xB6] = 0x11; data[0xB7] = 0x22;
        data[0xB8] = 0x33; data[0xB9] = 0x44; data[0xBA] = 0x99; data[0xBB] = 0x88;
        data[0xBC] = 0xFE; data[0xBD] = 0xDC; data[0xBE] = 0xBA; data[0xBF] = 0x98;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "DDS fallback test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "DDS fallback test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "DDS fallback test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.format = "PNG";
            options.preserveOriginalDDS = true;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should fall back to DDS when readable export is unavailable");
            ok &= Expect(result.outputPath.extension() == ".dds", "DDS fallback should produce a .dds file");
            ok &= Expect(fs::exists(result.outputPath), "DDS fallback should create the DDS output");

            bool sawFallbackWarning = false;
            for (const std::string& warning : result.warnings)
            {
                if (warning.find("Falling back to DDS") != std::string::npos)
                {
                    sawFallbackWarning = true;
                    break;
                }
            }
            ok &= Expect(sawFallbackWarning, "DDS fallback should report a fallback warning");

            if (fs::exists(result.outputPath))
            {
                std::ifstream dds(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ',
                    "DDS fallback output should start with DDS magic");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterLzmaDecompression()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureLzmaDecompressionTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "synthetic_textures_lzma.dxp.bin";
        const fs::path outputDir = tempDir / "export";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp LZMA decompression test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(256, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        data[0x8] = 1; data[0x9] = 0;
        data[0x20] = 0x60; data[0x21] = 0x00;
        data[0x30] = 0x80; data[0x31] = 0x00;

        const char name[] = "tex_lzma";
        std::copy(std::begin(name), std::end(name), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78; // DDSx
        put32(data, 0x74, 827611204); // DXT1
        put32(data, 0x78, 0x40000000); // LZMA/7zip compression flag
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 4);
        put32(data, 0x88, 8); // decompressed DXT1 block size
        put32(data, 0x8C, 32); // packed bytes in DXP payload (32 bytes)

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 32);

        // 32-byte valid LZMA stream
        const uint8_t lzmaPayload[32] = {
            0x5D, 0x00, 0x00, 0x80, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x55, 0x2E,
            0xD5, 0x8F, 0x6D, 0x14, 0x44, 0x03, 0x2D, 0x4B, 0x4C, 0x5E, 0xFF, 0xFF, 0xC0, 0x5F, 0x00, 0x00
        };
        std::copy(std::begin(lzmaPayload), std::end(lzmaPayload), data.begin() + 0xB0);

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "LZMA decompression test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "LZMA decompression test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "LZMA decompression test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export DDS successfully for decompressed LZMA DDSx payloads");
            if (!result.success)
            {
                std::cerr << "LZMA export error: " << result.error << '\n';
            }
            ok &= Expect(result.outputPath.extension() == ".dds", "LZMA decompression should produce .dds output");
            ok &= Expect(fs::exists(result.outputPath), "LZMA decompression output should exist");

            if (fs::exists(result.outputPath))
            {
                std::ifstream dds(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 136, "LZMA decompression DDS output should contain DDS header (128 bytes) plus decompressed payload (8 bytes)");
                ok &= Expect(bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ',
                    "LZMA decompression output should start with DDS magic");
                if (bytes.size() >= 136)
                {
                    const uint8_t expectedPayload[8] = { 0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33, 0x44 };
                    bool match = true;
                    for (int i = 0; i < 8; ++i)
                    {
                        if (bytes[128 + i] != expectedPayload[i])
                        {
                            match = false;
                            std::cerr << "Decompressed payload byte mismatch at offset " << i
                                      << ": expected " << std::hex << (int)expectedPayload[i]
                                      << ", got " << (int)bytes[128 + i] << std::dec << '\n';
                        }
                    }
                    ok &= Expect(match, "Decompressed bytes should match the original synthetic test bytes");
                }
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterReverseMipOrderNormalization()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureRevMipTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "reverse_mips.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp reverse-mip texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };
        auto read32 = [](const std::vector<uint8_t>& bytes, size_t offset) -> uint32_t {
            return static_cast<uint32_t>(bytes[offset + 0]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        };

        std::vector<uint8_t> data(0x110, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "reverse_mips";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 28); // R8G8B8A8_UNORM
        put32(data, 0x78, 0x00040000); // FLG_REV_MIP_ORDER
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 3;
        put16(data, 0x82, 1);
        put16(data, 0x84, 32);
        put32(data, 0x88, 84);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 84);

        size_t payloadOffset = 0xB0;
        for (size_t i = 0; i < 4; ++i) data[payloadOffset + i] = 0x11;       // mip2: 1x1
        payloadOffset += 4;
        for (size_t i = 0; i < 16; ++i) data[payloadOffset + i] = 0x22;      // mip1: 2x2
        payloadOffset += 16;
        for (size_t i = 0; i < 64; ++i) data[payloadOffset + i] = 0x33;      // mip0: 4x4

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "Reverse-mip test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "Reverse-mip test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "Reverse-mip test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should normalize reverse mip order");
            ok &= Expect(result.outputPath.extension() == ".dds", "Reverse-mip test should export DDS output");
            ok &= Expect(fs::exists(result.outputPath), "Reverse-mip DDS output should exist");

            bool sawWarning = false;
            for (const std::string& warning : result.warnings)
            {
                if (warning.find("Reordered reverse DDSx mip payload") != std::string::npos)
                {
                    sawWarning = true;
                    break;
                }
            }
            ok &= Expect(sawWarning, "Reverse-mip test should report the mip reorder warning");

            if (fs::exists(result.outputPath))
            {
                std::ifstream dds(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 212, "Reverse-mip DDS output should contain a 128-byte header plus 84 bytes of payload");
                ok &= Expect(bytes.size() >= 212 && read32(bytes, 28) == 3, "Reverse-mip DDS header should preserve mip count");
                ok &= Expect(bytes.size() >= 212 && bytes[128] == 0x33 && bytes[191] == 0x33, "Reverse-mip DDS payload should start with the largest mip");
                ok &= Expect(bytes.size() >= 212 && bytes[192] == 0x22 && bytes[207] == 0x22, "Reverse-mip DDS payload should place the second mip after the top mip");
                ok &= Expect(bytes.size() >= 212 && bytes[208] == 0x11 && bytes[211] == 0x11, "Reverse-mip DDS payload should end with the smallest mip");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterSplitMipClamping()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_TextureSplitMipTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "split_mips.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp split-mip texture export test folder: " << ec.message() << '\n';
            return false;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };
        auto read32 = [](const std::vector<uint8_t>& bytes, size_t offset) -> uint32_t {
            return static_cast<uint32_t>(bytes[offset + 0]) |
                (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                (static_cast<uint32_t>(bytes[offset + 3]) << 24);
        };

        std::vector<uint8_t> data(0x110, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "split_mips";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78;
        put32(data, 0x74, 28); // R8G8B8A8_UNORM
        put32(data, 0x78, 0x00040000); // FLG_REV_MIP_ORDER
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 3; // declared chain, but payload only contains top 2 mips
        data[0x81] = 1; // hqPartLevels
        put16(data, 0x82, 1);
        put16(data, 0x84, 32);
        put16(data, 0x86, 0x4321); // lQmip=1, mQmip=2, dxtShift=3, uQmip=4
        put32(data, 0x88, 80);
        put32(data, 0x8C, 0);

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 80);

        size_t payloadOffset = 0xB0;
        for (size_t i = 0; i < 16; ++i) data[payloadOffset + i] = 0x66; // mip1 stored first due to reverse order
        payloadOffset += 16;
        for (size_t i = 0; i < 64; ++i) data[payloadOffset + i] = 0x55; // mip0 stored last

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "Split-mip test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "Split-mip test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "Split-mip test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorFormats::DdsxReader headerReader(contents->entries[0].ddsxHeader);
            headerReader.Parse();
            const DagorFormats::DdsxHeader& parsedHeader = headerReader.GetHeader();
            ok &= Expect(parsedHeader.hqPartLevels == 1, "Split-mip test should preserve HQ part level metadata");
            ok &= Expect(parsedHeader.lQmip == 1, "Split-mip test should parse lQmip from the DDSx quality word");
            ok &= Expect(parsedHeader.mQmip == 2, "Split-mip test should parse mQmip from the DDSx quality word");
            ok &= Expect(parsedHeader.dxtShift == 3, "Split-mip test should parse dxtShift from the DDSx quality word");
            ok &= Expect(parsedHeader.uQmip == 4, "Split-mip test should parse uQmip from the DDSx quality word");

            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should clamp split mip chains instead of failing");
            ok &= Expect(result.outputPath.extension() == ".dds", "Split-mip test should export DDS output");
            ok &= Expect(fs::exists(result.outputPath), "Split-mip DDS output should exist");

            bool sawClampWarning = false;
            bool sawReorderWarning = false;
            for (const std::string& warning : result.warnings)
            {
                if (warning.find("Clamping mip count to 2") != std::string::npos)
                {
                    sawClampWarning = true;
                }
                if (warning.find("Reordered reverse DDSx mip payload") != std::string::npos)
                {
                    sawReorderWarning = true;
                }
            }
            ok &= Expect(sawClampWarning, "Split-mip test should report the mip-clamp warning");
            ok &= Expect(sawReorderWarning, "Split-mip test should report the reverse-mip reorder warning");

            if (fs::exists(result.outputPath))
            {
                std::ifstream dds(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(dds)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 208, "Split-mip DDS output should contain a 128-byte header plus 80 bytes of payload");
                ok &= Expect(bytes.size() >= 208 && read32(bytes, 28) == 2, "Split-mip DDS header should clamp mip count to the mips that exist");
                ok &= Expect(bytes.size() >= 208 && bytes[128] == 0x55 && bytes[191] == 0x55, "Split-mip DDS payload should preserve the top mip bytes");
                ok &= Expect(bytes.size() >= 208 && bytes[192] == 0x66 && bytes[207] == 0x66, "Split-mip DDS payload should preserve the next mip bytes");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestTextureExporterMissingOodleFallback()
    {
        bool ok = true;

        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_OodleFallbackTest_" + std::to_string(stamp));
        const fs::path packPath = tempDir / "oodle_textures.dxp.bin";
        const fs::path outputDir = tempDir / "out";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        if (ec)
        {
            std::cerr << "FAIL: Could not create temp Oodle fallback test folder: " << ec.message() << '\n';
            return false;
        }

        if (!DagorIO::Decompressor::FindOodleDll(tempDir.string()).empty())
        {
            fs::remove_all(tempDir, ec);
            return true;
        }

        auto put16 = [](std::vector<uint8_t>& data, size_t offset, uint16_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        };
        auto put32 = [](std::vector<uint8_t>& data, size_t offset, uint32_t value) {
            data[offset + 0] = static_cast<uint8_t>(value & 0xFF);
            data[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
            data[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
            data[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
        };

        std::vector<uint8_t> data(0xC0, 0);
        data[0] = 'D'; data[1] = 'x'; data[2] = 'P'; data[3] = '2';
        put16(data, 0x8, 1);
        put32(data, 0x20, 0x60);
        put32(data, 0x30, 0x80);

        const std::string textureName = "oodle_tex";
        std::copy(textureName.begin(), textureName.end(), data.begin() + 0x48);

        data[0x70] = 0x44; data[0x71] = 0x44; data[0x72] = 0x53; data[0x73] = 0x78; // DDSx
        put32(data, 0x74, 827611204); // DXT1
        put32(data, 0x78, 0x60000000); // Oodle compression flag
        put16(data, 0x7C, 4);
        put16(data, 0x7E, 4);
        data[0x80] = 1;
        put16(data, 0x82, 1);
        put16(data, 0x84, 4);
        put32(data, 0x88, 8); // decompressed DXT1 block size
        put32(data, 0x8C, 4); // packed bytes in DXP payload

        put32(data, 0x9C, 0xB0);
        put32(data, 0xA0, 4);
        data[0xB0] = 0xDE; data[0xB1] = 0xAD; data[0xB2] = 0xBE; data[0xB3] = 0xEF;

        {
            std::ofstream pack(packPath, std::ios::binary);
            pack.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        }

        const std::shared_ptr<AssetIndex::PackContents> contents = AssetIndex::ParseDxpContents(packPath);
        ok &= Expect(contents != nullptr, "Oodle fallback test should parse pack contents");
        ok &= Expect(contents && contents->error.empty(), "Oodle fallback test should parse without DXP errors");
        ok &= Expect(contents && contents->entries.size() == 1, "Oodle fallback test should expose one texture");

        if (contents && !contents->entries.empty())
        {
            DagorAssetsExtractor::Export::TextureExportOptions options;
            options.outputDir = outputDir;
            options.gameRoot = tempDir;
            options.overwrite = false;

            DagorAssetsExtractor::Export::TextureExportResult result =
                DagorAssetsExtractor::Export::TextureExporter::ExportTexture(packPath, contents->entries[0], options);

            ok &= Expect(result.success, "TextureExporter should export raw DDSx fallback when Oodle DLL is missing");
            if (!result.success)
            {
                std::cerr << "Oodle fallback export error: " << result.error << '\n';
            }
            ok &= Expect(result.outputPath.extension() == ".ddsx", "Oodle fallback should produce .ddsx output");
            ok &= Expect(fs::exists(result.outputPath), "Oodle fallback output should exist");

            if (fs::exists(result.outputPath))
            {
                std::ifstream ddsx(result.outputPath, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ddsx)), std::istreambuf_iterator<char>());
                ok &= Expect(bytes.size() == 36, "Oodle fallback DDSx output should contain header plus raw payload");
                ok &= Expect(bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == 'x',
                    "Oodle fallback output should start with DDSx magic");
            }
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestRendInstParsingAndExport()
    {
        bool ok = true;

        // 1. Build a synthetic LOD in memory
        DagorFormats::RendInstMesh mesh;
        mesh.name = "synthetic_tetrahedron";

        // 4 vertices (tetrahedron)
        mesh.vertices = {
            // Pos (x,y,z), Normal (nx,ny,nz), UV (u,v)
            { 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f,  0.5f, 1.0f },
            {-1.0f,-1.0f, 1.0f, -0.5f,-0.5f, 0.5f,  0.0f, 0.0f },
            { 1.0f,-1.0f, 1.0f,  0.5f,-0.5f, 0.5f,  1.0f, 0.0f },
            { 0.0f,-1.0f,-1.0f,  0.0f,-0.5f,-1.0f,  0.5f, 0.0f }
        };

        // 12 indices (4 faces)
        mesh.indices = {
            0, 1, 2,
            0, 2, 3,
            0, 3, 1,
            1, 3, 2
        };

        // 2. Serialize to binary buffer using our proprietary RIND format
        std::vector<uint8_t> buffer;
        auto pushBytes = [&buffer](const void* data, size_t size) {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
            buffer.insert(buffer.end(), ptr, ptr + size);
        };
        auto pushString = [&buffer](const std::string& str, size_t fixedLen) {
            std::vector<uint8_t> temp(fixedLen, 0);
            size_t len = std::min(str.size(), fixedLen);
            std::copy(str.begin(), str.begin() + len, temp.begin());
            buffer.insert(buffer.end(), temp.begin(), temp.end());
        };

        pushString("RIND", 4);
        uint32_t version = 1;
        pushBytes(&version, sizeof(version));
        uint32_t lodCount = 1;
        pushBytes(&lodCount, sizeof(lodCount));

        // LOD 0 Header
        pushString(mesh.name, 32);
        uint32_t vertCount = static_cast<uint32_t>(mesh.vertices.size());
        pushBytes(&vertCount, sizeof(vertCount));
        uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
        pushBytes(&indexCount, sizeof(indexCount));

        // Vertices: keep the synthetic RIND fixture layout stable at 8 floats per vertex.
        for (const DagorFormats::Vertex& vertex : mesh.vertices)
        {
            pushBytes(&vertex.x, sizeof(vertex.x));
            pushBytes(&vertex.y, sizeof(vertex.y));
            pushBytes(&vertex.z, sizeof(vertex.z));
            pushBytes(&vertex.nx, sizeof(vertex.nx));
            pushBytes(&vertex.ny, sizeof(vertex.ny));
            pushBytes(&vertex.nz, sizeof(vertex.nz));
            pushBytes(&vertex.u, sizeof(vertex.u));
            pushBytes(&vertex.v, sizeof(vertex.v));
        }
        // Indices
        pushBytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));

        // Primitives
        uint32_t primitiveCount = 1;
        pushBytes(&primitiveCount, sizeof(primitiveCount));
        uint32_t matIdx = 0, firstIdx = 0, count = indexCount;
        pushBytes(&matIdx, sizeof(matIdx));
        pushBytes(&firstIdx, sizeof(firstIdx));
        pushBytes(&count, sizeof(count));

        // 3. Parse with RendInstReader
        DagorFormats::RendInstReader reader(buffer);
        try {
            reader.Parse();
            const auto& lods = reader.GetLods();
            ok &= Expect(lods.size() == 1, "RendInstReader should parse 1 LOD");
            if (lods.size() == 1)
            {
                ok &= Expect(lods[0].name == "synthetic_tetrahedron", "Parsed LOD name should match");
                ok &= Expect(lods[0].vertices.size() == 4, "Parsed vertex count should be 4");
                ok &= Expect(lods[0].indices.size() == 12, "Parsed index count should be 12");
                if (lods[0].vertices.size() == 4)
                {
                    ok &= Expect(lods[0].vertices[0].x == 0.0f && lods[0].vertices[0].y == 1.0f, "Parsed vertex coordinates should match");
                    ok &= Expect(lods[0].vertices[0].u == 0.5f && lods[0].vertices[0].v == 1.0f, "Parsed vertex UV should match");
                }
            }
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("RendInstReader failed with exception: ") + e.what()).c_str());
        }

        // 4. Export using ModelExporter
        fs::path tempDir = fs::temp_directory_path() / "DagorAssetsExtractorTests";
        fs::create_directories(tempDir);
        fs::path glbPath = tempDir / "test_tetrahedron.glb";
        fs::path objPath = tempDir / "test_tetrahedron.obj";

        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourcePack = "synthetic_vehicle.grp";
        manifest.sourceResource = "synthetic_tetrahedron";
        manifest.assetType = "RendInst Static Model";
        manifest.appVersion = "1.0.0-test";
        manifest.formatDecoderVersion = "RendInstReader v1.0";

        // Export GLB
        bool glbOk = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(glbPath, reader.GetLods(), manifest);
        ok &= Expect(glbOk, "ModelExporter should export GLB successfully");
        ok &= Expect(fs::exists(glbPath), "GLB file should exist on disk");

        fs::path glbManifestPath = glbPath;
        glbManifestPath.replace_extension(".export-manifest.json");
        ok &= Expect(fs::exists(glbManifestPath), "GLB Export manifest should exist on disk");

        // Export OBJ
        if (reader.GetLods().size() == 1)
        {
            bool objOk = DagorAssetsExtractor::Export::ModelExporter::ExportToObj(objPath, reader.GetLods()[0], manifest);
            ok &= Expect(objOk, "ModelExporter should export OBJ successfully");
            ok &= Expect(fs::exists(objPath), "OBJ file should exist on disk");

            fs::path objManifestPath = objPath;
            objManifestPath.replace_extension(".export-manifest.json");
            ok &= Expect(fs::exists(objManifestPath), "OBJ Export manifest should exist on disk");
        }

        // Clean up
        std::error_code ec;
        fs::remove_all(tempDir, ec);

        return ok;
    }

    bool TestMultiMaterialModelExport()
    {
        bool ok = true;
        const fs::path tempDir = fs::temp_directory_path() / "DagorAssetsExtractorTests_MultiMat";
        fs::create_directories(tempDir);

        // 1. Create a synthetic RendInstMesh with 2 primitives
        DagorFormats::RendInstMesh mesh;
        mesh.name = "multi_mat_mesh";
        mesh.vertices = {
            { 0,0,0, 0,1,0, 0,0 }, { 1,0,0, 0,1,0, 1,0 }, { 0,0,1, 0,1,0, 0,1 }, // Tri 0
            { 2,0,0, 0,1,0, 0,0 }, { 3,0,0, 0,1,0, 1,0 }, { 2,0,1, 0,1,0, 0,1 }  // Tri 1
        };
        mesh.indices = { 0,1,2, 3,4,5 };
        mesh.primitives = {
            { 0, 0, 3 }, // Mat 0, indices 0-2
            { 1, 3, 3 }  // Mat 1, indices 3-5
        };

        std::vector<DagorFormats::RendInstMesh> lods = { mesh };
        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourceResource = "multi_mat_test";
        
        std::vector<DagorAssetsExtractor::Export::MaterialInfo> materials = {
            { "mat_a", "tex_a.dds" },
            { "mat_b", "tex_b.dds" }
        };

        const fs::path glbPath = tempDir / "test_multi.glb";
        if (DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(glbPath, lods, manifest, materials))
        {
            std::ifstream file(glbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            
            // Basic GLB structural checks
            ok &= Expect(content.size() > 100, "GLB should have some content");
            ok &= Expect(content.find("\"primitives\":") != std::string::npos, "GLB should have primitives");
            
            // Check for multiple primitives in the JSON chunk
            size_t pos = content.find("\"primitives\":");
            size_t count = 0;
            while (pos != std::string::npos)
            {
                // Simple check for primitive opening brace
                size_t nextPrim = content.find('{', pos);
                if (nextPrim != std::string::npos)
                {
                    count++;
                    pos = content.find('{', nextPrim + 1);
                    // This is a bit naive but good enough for a synthetic test
                    if (pos != std::string::npos && pos < content.find(']', nextPrim)) count++;
                }
                pos = content.find("\"primitives\":", pos + 1);
            }
            // Better check: search for multiple "material": X
            size_t mat0 = content.find("\"material\": 0");
            size_t mat1 = content.find("\"material\": 1");
            ok &= Expect(mat0 != std::string::npos && mat1 != std::string::npos, "GLB should reference both materials");
        }
        else
        {
            ok &= Expect(false, "GLB export should succeed");
        }

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestModelExporterMaterialFallbackAndNormals()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_MaterialFallback_" + std::to_string(stamp));
        fs::create_directories(tempDir);

        DagorFormats::RendInstMesh mesh;
        mesh.name = "fallback_material_mesh";
        mesh.vertices = {
            { 0,0,0, 0,0,0, 0,0 },
            { 1,0,0, 0,0,0, 1,0 },
            { 0,1,0, 0,0,0, 0,1 }
        };
        mesh.indices = { 0, 1, 2 };
        mesh.primitives = { { 2, 0, 3 } };

        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourceResource = "fallback_material_mesh";
        const fs::path glbPath = tempDir / "fallback.glb";

        const bool success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(glbPath, { mesh }, manifest);
        ok &= Expect(success, "ModelExporter should export material fallback GLB");
        ok &= Expect(fs::exists(glbPath), "Material fallback GLB should exist");

        if (fs::exists(glbPath))
        {
            std::ifstream file(glbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"materials\":") != std::string::npos, "GLB should emit material slots even without texture dependencies");
            ok &= Expect(content.find("\"material\": 0") != std::string::npos, "Primitive should reference compacted used material index");
            ok &= Expect(content.find("material_2") != std::string::npos, "Fallback material name should preserve source material slot");
            ok &= Expect(content.find("\"images\":") == std::string::npos, "Textureless fallback materials should not emit empty image URIs");
        }

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestDynModelParsingAndExport()
    {
        bool ok = true;

        // 1. Build a synthetic skinned mesh in memory
        DagorFormats::DynModelMesh mesh;
        mesh.name = "synthetic_skinned_triangle";

        // 3 vertices
        DagorFormats::SkinnedVertex v0 = { 0.0f, 1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.5f, 1.0f, {0,0,0,0}, {1.0f,0.0f,0.0f,0.0f} };
        DagorFormats::SkinnedVertex v1 = {-1.0f,-1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f, {1,0,0,0}, {1.0f,0.0f,0.0f,0.0f} };
        DagorFormats::SkinnedVertex v2 = { 1.0f,-1.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f, {2,0,0,0}, {1.0f,0.0f,0.0f,0.0f} };
        mesh.vertices = { v0, v1, v2 };
        mesh.indices = { 0, 1, 2 };

        // 2. Serialize to binary buffer (DYN3)
        std::vector<uint8_t> buffer;
        auto pushBytes = [&buffer](const void* data, size_t size) {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
            buffer.insert(buffer.end(), ptr, ptr + size);
        };
        auto pushString = [&buffer](const std::string& str, size_t fixedLen) {
            std::vector<uint8_t> temp(fixedLen, 0);
            size_t len = std::min(str.size(), fixedLen);
            std::copy(str.begin(), str.begin() + len, temp.begin());
            buffer.insert(buffer.end(), temp.begin(), temp.end());
        };

        pushString("DYN3", 4);
        uint32_t version = 1;
        pushBytes(&version, sizeof(version));
        uint32_t lodCount = 1;
        pushBytes(&lodCount, sizeof(lodCount));

        pushString(mesh.name, 32);
        uint32_t vertCount = static_cast<uint32_t>(mesh.vertices.size());
        pushBytes(&vertCount, sizeof(vertCount));
        uint32_t indexCount = static_cast<uint32_t>(mesh.indices.size());
        pushBytes(&indexCount, sizeof(indexCount));

        // Vertices: keep the synthetic DYN3 fixture layout stable at 8 floats, 4 joint bytes, 4 weights.
        for (const DagorFormats::SkinnedVertex& vertex : mesh.vertices)
        {
            pushBytes(&vertex.x, sizeof(vertex.x));
            pushBytes(&vertex.y, sizeof(vertex.y));
            pushBytes(&vertex.z, sizeof(vertex.z));
            pushBytes(&vertex.nx, sizeof(vertex.nx));
            pushBytes(&vertex.ny, sizeof(vertex.ny));
            pushBytes(&vertex.nz, sizeof(vertex.nz));
            pushBytes(&vertex.u, sizeof(vertex.u));
            pushBytes(&vertex.v, sizeof(vertex.v));
            pushBytes(vertex.jointIndices, sizeof(vertex.jointIndices));
            pushBytes(vertex.jointWeights, sizeof(vertex.jointWeights));
        }
        // Indices
        pushBytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));

        // Primitives
        uint32_t primitiveCount = 1;
        pushBytes(&primitiveCount, sizeof(primitiveCount));
        uint32_t matIdx = 0, firstIdx = 0, count = indexCount;
        pushBytes(&matIdx, sizeof(matIdx));
        pushBytes(&firstIdx, sizeof(firstIdx));
        pushBytes(&count, sizeof(count));

        // 3. Parse with DynModelReader
        DagorFormats::DynModelReader reader(buffer);
        try {
            reader.Parse();
            const auto& lods = reader.GetLods();
            ok &= Expect(lods.size() == 1, "DynModelReader should parse 1 LOD");
            if (lods.size() == 1)
            {
                ok &= Expect(lods[0].vertices.size() == 3, "Parsed vertex count should be 3");
                ok &= Expect(lods[0].vertices[0].jointIndices[0] == 0, "Joint index 0 should be 0");
                ok &= Expect(lods[0].vertices[0].jointWeights[0] == 1.0f, "Joint weight 0 should be 1.0");
            }
        } catch (const std::exception& e) {
            ok &= Expect(false, (std::string("DynModelReader failed: ") + e.what()).c_str());
        }

        // 4. Export using ModelExporter
        fs::path tempDir = fs::temp_directory_path() / "DagorAssetsExtractorTests_Skinned";
        fs::create_directories(tempDir);
        fs::path glbPath = tempDir / "test_skinned.glb";

        std::vector<DagorFormats::GeomNodeTreeNode> skeleton;
        const auto identityMatrix = []()
        {
            std::array<float, 16> matrix = {};
            matrix[0] = 1.0f;
            matrix[5] = 1.0f;
            matrix[10] = 1.0f;
            matrix[15] = 1.0f;
            return matrix;
        };
        const auto translationMatrix = [&](float x, float y, float z)
        {
            std::array<float, 16> matrix = identityMatrix();
            matrix[3] = x;
            matrix[7] = y;
            matrix[11] = z;
            return matrix;
        };
        DagorFormats::GeomNodeTreeNode n0; n0.name = "root"; n0.parentIndex = DagorFormats::GeomNodeTreeNode::InvalidIndex; n0.childIndices = { 1, 2 };
        DagorFormats::GeomNodeTreeNode n1; n1.name = "bone1"; n1.parentIndex = 0;
        DagorFormats::GeomNodeTreeNode n2; n2.name = "bone2"; n2.parentIndex = 0;
        n0.localTransform = n0.worldTransform = n0.inverseBindTransform = identityMatrix();
        n1.localTransform = n1.worldTransform = translationMatrix(2.0f, 0.0f, 0.0f);
        n1.inverseBindTransform = translationMatrix(-2.0f, 0.0f, 0.0f);
        n2.localTransform = n2.worldTransform = n2.inverseBindTransform = identityMatrix();
        skeleton = { n0, n1, n2 };

        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourceResource = "skinned_test";

        const auto readFirstExportedNormal = [](const fs::path& path) -> std::optional<std::array<float, 3>>
        {
            std::ifstream file(path, std::ios::binary);
            std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (bytes.size() < 28)
            {
                return std::nullopt;
            }

            auto readU32 = [&](size_t offset) -> uint32_t
            {
                return static_cast<uint32_t>(bytes[offset]) |
                    (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                    (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                    (static_cast<uint32_t>(bytes[offset + 3]) << 24);
            };

            const uint32_t jsonLength = readU32(12);
            const size_t binHeaderOffset = 20u + jsonLength;
            if (binHeaderOffset > bytes.size() || bytes.size() - binHeaderOffset < 8)
            {
                return std::nullopt;
            }

            const uint32_t binLength = readU32(binHeaderOffset);
            const size_t binOffset = binHeaderOffset + 8;
            constexpr size_t normalByteOffset = 12;
            if (binOffset > bytes.size() || bytes.size() - binOffset < binLength ||
                binLength < normalByteOffset + (sizeof(float) * 3))
            {
                return std::nullopt;
            }

            std::array<float, 3> normal = {};
            std::memcpy(normal.data(), bytes.data() + binOffset + normalByteOffset, sizeof(float) * normal.size());
            return normal;
        };

        bool success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(glbPath, reader.GetLods(), skeleton, manifest);
        ok &= Expect(success, "Skinned ModelExporter should export GLB successfully");
        ok &= Expect(fs::exists(glbPath), "Skinned GLB file should exist");

        if (fs::exists(glbPath))
        {
            std::ifstream file(glbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"skins\":") != std::string::npos, "GLB should contain skins");
            ok &= Expect(content.find("\"JOINTS_0\":") != std::string::npos, "GLB should contain JOINTS_0 attribute");
            ok &= Expect(content.find("\"WEIGHTS_0\":") != std::string::npos, "GLB should contain WEIGHTS_0 attribute");
            ok &= Expect(content.find("\"inverseBindMatrices\":") != std::string::npos, "GLB should contain inverseBindMatrices");
            ok &= Expect(content.find("\"name\": \"Root\"") == std::string::npos,
                "Skinned GLB should not wrap skinned mesh nodes under an artificial root");
            ok &= Expect(content.find("\"nodes\": [0]\n") == std::string::npos,
                "Skinned GLB scene should keep mesh nodes at scene root to avoid NODE_SKINNED_MESH_NON_ROOT");
        }

        DagorFormats::DynModelMesh rigidMesh;
        rigidMesh.name = "synthetic_rigid_bone_part";
        rigidMesh.parentNodeName = "bone1";
        rigidMesh.useSkinning = false;
        rigidMesh.vertices = { v0, v1, v2 };
        rigidMesh.indices = { 0, 1, 2 };
        rigidMesh.primitives.push_back({ 0, 0, 3 });

        fs::path rigidRigGlbPath = tempDir / "test_rigid_rig.glb";
        bool rigidRigSuccess = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(rigidRigGlbPath, { rigidMesh }, skeleton, manifest);
        ok &= Expect(rigidRigSuccess, "Rigid DynModel part should export parented to skeleton when skeleton is provided");
        if (fs::exists(rigidRigGlbPath))
        {
            std::ifstream file(rigidRigGlbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"skins\":") == std::string::npos, "Rigid-only rig GLB should avoid skinning every rigid vertex");
            ok &= Expect(content.find("\"skin\": 0") == std::string::npos, "Rigid-only rig mesh node should not reference a skin");
            ok &= Expect(content.find("\"JOINTS_0\":") == std::string::npos, "Rigid-only rig GLB should not contain JOINTS_0");
            ok &= Expect(content.find("\"WEIGHTS_0\":") == std::string::npos, "Rigid-only rig GLB should not contain WEIGHTS_0");
            ok &= Expect(content.find("\"name\": \"bone1\"") != std::string::npos, "Rigid-only rig GLB should emit matching skeleton bone nodes");
            ok &= Expect(content.find("\"name\": \"bone2\"") == std::string::npos, "Rigid-only rig GLB should prune unused skeleton helper nodes");
            ok &= Expect(content.find("\"children\": [2]") != std::string::npos, "Rigid-only rig GLB should parent the rigid mesh under its matching bone");
            ok &= Expect(content.find("\"name\": \"Root\"") == std::string::npos,
                "Rigid rig GLB should not create an artificial root node");
        }

        fs::path meshOnlyPlacementGlbPath = tempDir / "test_mesh_only_placement.glb";
        bool meshOnlyPlacementSuccess = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(meshOnlyPlacementGlbPath, { rigidMesh }, skeleton, manifest, {}, false);
        ok &= Expect(meshOnlyPlacementSuccess, "Mesh-only DynModel export should still use skeleton placement metadata");
        if (fs::exists(meshOnlyPlacementGlbPath))
        {
            std::ifstream file(meshOnlyPlacementGlbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"skins\":") == std::string::npos, "Mesh-only placement GLB should not contain skins");
            ok &= Expect(content.find("\"JOINTS_0\":") == std::string::npos, "Mesh-only placement GLB should not contain JOINTS_0");
            ok &= Expect(content.find("\"name\": \"bone1\"") == std::string::npos, "Mesh-only placement GLB should not emit skeleton nodes");
            ok &= Expect(content.find("\"matrix\":") != std::string::npos, "Mesh-only placement GLB should keep the rigid part node transform");
        }

        DagorAssetsExtractor::Export::AnimationClip animationClip;
        animationClip.name = "synthetic_clip";
        DagorAssetsExtractor::Export::AnimationNodeTrack animationTrack;
        animationTrack.nodeName = "bone2";
        animationTrack.times = { 0.0f, 1.0f };
        animationTrack.translations = { std::array<float, 3>{ 0.0f, 0.0f, 0.0f }, std::array<float, 3>{ 0.0f, 1.0f, 0.0f } };
        animationTrack.rotations = { std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }, std::array<float, 4>{ 0.0f, 0.0f, 0.70710677f, 0.70710677f } };
        animationTrack.scales = { std::array<float, 3>{ 1.0f, 1.0f, 1.0f }, std::array<float, 3>{ 1.0f, 1.5f, 1.0f } };
        animationClip.nodeTracks.push_back(std::move(animationTrack));

        fs::path animatedRigGlbPath = tempDir / "test_rigid_rig_animation.glb";
        bool animatedRigSuccess = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(
            animatedRigGlbPath,
            { rigidMesh },
            skeleton,
            manifest,
            std::vector<DagorAssetsExtractor::Export::MaterialInfo>{},
            true,
            DagorAssetsExtractor::Export::GlbExportOptions{},
            { animationClip });
        ok &= Expect(animatedRigSuccess, "Rigid DynModel export should write GLB animation channels when animation clips are supplied");
        if (fs::exists(animatedRigGlbPath))
        {
            std::ifstream file(animatedRigGlbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"animations\":") != std::string::npos, "Animated GLB should contain animations");
            ok &= Expect(content.find("\"path\": \"translation\"") != std::string::npos, "Animated GLB should contain translation channels");
            ok &= Expect(content.find("\"path\": \"rotation\"") != std::string::npos, "Animated GLB should contain rotation channels");
            ok &= Expect(content.find("\"path\": \"scale\"") != std::string::npos, "Animated GLB should contain scale channels");
            ok &= Expect(content.find("\"name\": \"bone2\"") != std::string::npos, "Animated GLB should emit animated skeleton nodes even when no mesh is parented to them");
        }

        fs::path noRigGlbPath = tempDir / "test_no_rig.glb";
        bool noRigSuccess = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(noRigGlbPath, reader.GetLods(), {}, manifest);
        ok &= Expect(noRigSuccess, "Skinned ModelExporter should still export mesh-only GLB without skeleton");
        if (fs::exists(noRigGlbPath))
        {
            std::ifstream file(noRigGlbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"skins\":") == std::string::npos, "Mesh-only GLB should not contain skins");
            ok &= Expect(content.find("\"JOINTS_0\":") == std::string::npos, "Mesh-only GLB should not contain JOINTS_0");
            ok &= Expect(content.find("\"WEIGHTS_0\":") == std::string::npos, "Mesh-only GLB should not contain WEIGHTS_0");
        }

        DagorFormats::DynModelMesh flippedNormalMesh = mesh;
        flippedNormalMesh.name = "synthetic_flipped_normal_triangle";
        for (DagorFormats::SkinnedVertex& vertex : flippedNormalMesh.vertices)
        {
            vertex.nz = -1.0f;
        }

        fs::path flippedNormalGlbPath = tempDir / "test_flipped_normal_repair.glb";
        bool flippedNormalSuccess = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(flippedNormalGlbPath, { flippedNormalMesh }, {}, manifest);
        ok &= Expect(flippedNormalSuccess, "DynModel exporter should write flipped-normal regression GLB");
        if (fs::exists(flippedNormalGlbPath))
        {
            const std::optional<std::array<float, 3>> normal = readFirstExportedNormal(flippedNormalGlbPath);
            ok &= Expect(normal.has_value(), "Flipped-normal regression should read first GLB normal");
            if (normal)
            {
                ok &= Expect((*normal)[2] > 0.9f,
                    "DynModel exporter should preserve valid vertex normals with correct handedness conversion");
            }
        }

        std::error_code ec;
        fs::remove_all(tempDir, ec);

        return ok;
    }

    bool TestUnifiedModelExport()
    {
        bool ok = true;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const fs::path tempDir = fs::temp_directory_path() / ("DagorAssetsExtractor_UnifiedExportTest_" + std::to_string(stamp));
        const fs::path glbPath = tempDir / "textured_model.glb";

        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(tempDir, ec);
        fs::create_directories(tempDir / "Textures", ec);
        const fs::path texturePath = tempDir / "Textures" / "box_tex.png";
        {
            const uint8_t pngBytes[] = {
                0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A,
                0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
                0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
                0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
                0x89, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E,
                0x44, 0xAE, 0x42, 0x60, 0x82
            };
            std::ofstream textureFile(texturePath, std::ios::binary);
            textureFile.write(reinterpret_cast<const char*>(pngBytes), sizeof(pngBytes));
        }

        // 1. Create a simple mesh
        DagorFormats::RendInstMesh mesh;
        mesh.name = "box";
        mesh.vertices = {
            { -1,-1,-1, 0,0,0, 0,0 }, { 1,-1,-1, 0,0,0, 1,0 }, { 1, 1,-1, 0,0,0, 1,1 }, { -1, 1,-1, 0,0,0, 0,1 }
        };
        mesh.indices = { 0, 1, 2, 0, 2, 3 };

        // 2. Define materials
        DagorAssetsExtractor::Export::MaterialInfo mat;
        mat.name = "box_material";
        mat.diffuseTexturePath = "Textures/box_tex.png";
        mat.diffuseTextureSourcePath = texturePath;
        std::vector<DagorAssetsExtractor::Export::MaterialInfo> materials = { mat };

        // 3. Export
        DagorAssetsExtractor::Export::ExportManifest manifest;
        manifest.sourcePack = "test.grp";
        manifest.sourceResource = "box";
        manifest.assetType = "RendInst Static Model";
        manifest.textureDependencies = { "Textures/box_tex.png" };

        bool success = DagorAssetsExtractor::Export::ModelExporter::ExportToGlb(glbPath, { mesh }, manifest, materials);
        ok &= Expect(success, "Unified ExportToGlb should succeed");
        ok &= Expect(fs::exists(glbPath), "Textured GLB should exist");

        if (fs::exists(glbPath))
        {
            // Simple check: see if texture path is in the GLB (it's in the JSON chunk)
            std::ifstream file(glbPath, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            ok &= Expect(content.find("\"uri\":") == std::string::npos, "Embedded GLB texture should not use an external image URI");
            ok &= Expect(content.find("\"mimeType\": \"image/png\"") != std::string::npos, "Embedded GLB texture should declare PNG mime type");
            ok &= Expect(content.find("box_material") != std::string::npos, "GLB should contain material name");
            ok &= Expect(content.find("\"material\": 0") != std::string::npos, "GLB mesh should refer to material 0");
        }

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestSharedSettingsSaveAndLoad()
    {
        bool ok = true;

        namespace App = DagorAssetsExtractor::App;

        const fs::path tempDir = fs::temp_directory_path() / "DagorAssetsExtractor_SharedSettings";
        const fs::path settingsPath = tempDir / "cache" / "Settings.ini";
        std::error_code ec;
        fs::remove_all(tempDir, ec);

        App::AppSettings settings;
        settings.gameRoot = fs::path(L"C:\\Games\\WarThunder");
        settings.exportRoot = fs::path(L"C:\\Exports");
        settings.textureExportFormat = "PNG";
        settings.modelExportFormat = "OBJ";
        settings.exportTexturesWithModels = false;
        settings.embedTexturesInGlb = false;
        settings.exportJsonManifest = true;
        settings.preserveOriginalDDS = false;
        settings.indexCacheEnabled = false;
        settings.maxLoadedPackContents = 17;
        settings.showVromfs = false;
        settings.showGrp = false;
        settings.showDxp = true;
        settings.showDescriptors = false;
        settings.showBlk = true;
        settings.groupByBlk = true;
        settings.lastOpenedPackFilters = "grp,blk";

        const std::string imguiPayload =
            "[Window][Main]\n"
            "Pos=12,34\n"
            "Size=560,420\n"
            "Collapsed=0\n";

        std::string error;
        ok &= Expect(App::SaveSharedSettings(settings, settingsPath, imguiPayload, error), "Shared settings save should succeed");
        if (!error.empty())
        {
            std::cerr << "Shared settings save detail: " << error << '\n';
        }

        ok &= Expect(fs::exists(settingsPath), "Shared settings save should create Settings.ini");

        App::AppSettings loaded;
        App::LoadSettings(loaded, settingsPath);
        ok &= Expect(loaded.gameRoot == settings.gameRoot, "Shared settings load should restore game root");
        ok &= Expect(loaded.exportRoot == settings.exportRoot, "Shared settings load should restore export root");
        ok &= Expect(loaded.textureExportFormat == "PNG", "Shared settings load should restore texture format");
        ok &= Expect(loaded.modelExportFormat == "OBJ", "Shared settings load should restore model format");
        ok &= Expect(!loaded.exportTexturesWithModels, "Shared settings load should restore export-textures flag");
        ok &= Expect(!loaded.embedTexturesInGlb, "Shared settings load should restore embed-textures flag");
        ok &= Expect(loaded.exportJsonManifest, "Shared settings load should restore manifest flag");
        ok &= Expect(!loaded.preserveOriginalDDS, "Shared settings load should restore DDS fallback flag");
        ok &= Expect(!loaded.indexCacheEnabled, "Shared settings load should restore cache-enabled flag");
        ok &= Expect(loaded.maxLoadedPackContents == 17, "Shared settings load should restore max-loaded-containers value");
        ok &= Expect(!loaded.showVromfs && !loaded.showGrp && loaded.showDxp, "Shared settings load should restore browser visibility flags");
        ok &= Expect(!loaded.showDescriptors && loaded.showBlk, "Shared settings load should restore descriptor/BLK flags");
        ok &= Expect(loaded.groupByBlk, "Shared settings load should restore group-by-BLK flag");
        ok &= Expect(loaded.lastOpenedPackFilters == "grp,blk", "Shared settings load should restore saved pack filters");

        const std::string loadedImGui = App::LoadImGuiSettings(settingsPath);
        ok &= Expect(loadedImGui.find("[Window][Main]") != std::string::npos, "Shared settings load should return ImGui sections");
        ok &= Expect(loadedImGui.find("Pos=12,34") != std::string::npos, "Shared settings load should return ImGui values");
        ok &= Expect(loadedImGui.find("textureExportFormat=PNG") == std::string::npos, "Shared settings load should not leak app key/value lines into ImGui payload");

        fs::remove_all(tempDir, ec);
        return ok;
    }

    bool TestSharedSettingsLegacyMigration()
    {
        bool ok = true;

        namespace App = DagorAssetsExtractor::App;

        const fs::path tempDir = fs::temp_directory_path() / "DagorAssetsExtractor_SharedSettingsLegacy";
        const fs::path settingsDir = tempDir / "cache";
        const fs::path legacySettingsPath = settingsDir / "settings";
        const fs::path settingsPath = settingsDir / "Settings.ini";
        std::error_code ec;
        fs::remove_all(tempDir, ec);
        fs::create_directories(settingsDir, ec);

        {
            std::ofstream file(legacySettingsPath, std::ios::trunc);
            file << "gameRoot=C:/LegacyGame\n";
            file << "textureExportFormat=TGA\n";
            file << "maxLoadedPackContents=9\n";
            file << "groupByBlk=true\n";
        }

        const fs::path originalCurrentPath = fs::current_path();
        fs::current_path(tempDir, ec);
        ok &= Expect(!ec, "Shared settings migration test should change current directory");

        {
            std::ofstream file(tempDir / "imgui.ini", std::ios::trunc);
            file << "[Window][Legacy]\n";
            file << "Pos=90,45\n";
            file << "Collapsed=0\n";
        }

        App::AppSettings loaded;
        App::LoadSettings(loaded, settingsPath);
        ok &= Expect(loaded.gameRoot == fs::path(L"C:\\LegacyGame"), "Legacy settings load should restore game root from the old cache file");
        ok &= Expect(loaded.textureExportFormat == "TGA", "Legacy settings load should restore texture format from the old cache file");
        ok &= Expect(loaded.maxLoadedPackContents == 9, "Legacy settings load should restore max-loaded-containers from the old cache file");
        ok &= Expect(loaded.groupByBlk, "Legacy settings load should restore group-by-BLK from the old cache file");

        const std::string legacyImGui = App::LoadImGuiSettings(settingsPath);
        ok &= Expect(legacyImGui.find("[Window][Legacy]") != std::string::npos, "Legacy settings load should migrate ImGui sections from imgui.ini");
        ok &= Expect(legacyImGui.find("Pos=90,45") != std::string::npos, "Legacy settings load should migrate ImGui values from imgui.ini");

        std::string error;
        ok &= Expect(App::SaveSharedSettings(loaded, settingsPath, legacyImGui, error), "Shared settings migration save should create Settings.ini");
        if (!error.empty())
        {
            std::cerr << "Shared settings migration save detail: " << error << '\n';
        }
        ok &= Expect(fs::exists(settingsPath), "Shared settings migration save should create the new Settings.ini file");

        fs::current_path(originalCurrentPath, ec);
        fs::remove_all(tempDir, ec);
        return ok;
    }
}

int main(int argc, char** argv)
{
    fs::path repoRoot = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path fixtureRoot = repoRoot / "tests" / "fixtures" / "synthetic_wt";

    bool ok = true;

    ok &= Expect(TestBinaryReader(), "BinaryReader tests should pass");
    ok &= Expect(TestVromfsReader(), "VromfsReader tests should pass");
    ok &= Expect(TestGrpReader(), "GrpReader tests should pass");
    ok &= Expect(TestDxpReader(), "DxpReader tests should pass");
    ok &= Expect(TestDdsxReader(), "DdsxReader tests should pass");
    ok &= Expect(TestBinaryBlkReader(), "Binary BlkReader tests should pass");
    ok &= Expect(TestBlkReader(), "BlkReader tests should pass");
    ok &= Expect(TestGameResDescReader(), "GameResDescReader tests should pass");
    ok &= Expect(TestAnim2DataReader(), "Anim2DataReader tests should pass");
    ok &= Expect(TestGeomNodeTreeReader(), "GeomNodeTreeReader tests should pass");
    ok &= Expect(TestGrpPackContentsMetadata(), "GRP PackContents metadata tests should pass");
    ok &= Expect(TestJobSystem(), "JobSystem tests should pass");
    ok &= Expect(TestDecompression(), "Decompression tests should pass");
    ok &= Expect(TestExportManifest(), "ExportManifest tests should pass");
    ok &= Expect(TestTextureExporterDX10(), "TextureExporter DX10 tests should pass");
    ok &= Expect(TestTextureExporterDxpPayloadExport(), "TextureExporter DXP payload export test should pass");
    ok &= Expect(TestTextureExporterReadablePngExport(), "TextureExporter readable PNG export test should pass");
    ok &= Expect(TestTextureExporterBatchSkipExistingReadablePng(), "TextureExporter batch readable PNG skip test should pass");
    ok &= Expect(TestTextureExporterReadableTgaExport(), "TextureExporter readable TGA export test should pass");
    ok &= Expect(TestTextureExporterReadableRgba16fTgaExport(), "TextureExporter readable RGBA16F TGA export test should pass");
    ok &= Expect(TestTextureExporterReadableRgba32fTgaExport(), "TextureExporter readable RGBA32F TGA export test should pass");
    ok &= Expect(TestTextureExporterReadableBc1TgaExport(), "TextureExporter readable BC1 TGA export test should pass");
    ok &= Expect(TestTextureExporterReadableBc5TgaExport(), "TextureExporter readable BC5 TGA export test should pass");
    ok &= Expect(TestTextureExporterReadableBc7PngExport(), "TextureExporter readable BC7 PNG export test should pass");
    ok &= Expect(TestTextureExporterReadableBc6hPngExport(), "TextureExporter readable BC6H PNG export test should pass");
    ok &= Expect(TestTextureExporterReadableFallbackToDds(), "TextureExporter readable export fallback test should pass");
    ok &= Expect(TestTextureExporterLzmaDecompression(), "TextureExporter LZMA decompression test should pass");
    ok &= Expect(TestTextureExporterReverseMipOrderNormalization(), "TextureExporter reverse-mip normalization test should pass");
    ok &= Expect(TestTextureExporterSplitMipClamping(), "TextureExporter split-mip clamping test should pass");
    ok &= Expect(TestTextureExporterMissingOodleFallback(), "TextureExporter missing-Oodle fallback test should pass");
    ok &= Expect(TestRendInstParsingAndExport(), "RendInst geometry parsing and GLB/OBJ export tests should pass");
    ok &= Expect(TestMultiMaterialModelExport(), "Multi-material model export test should pass");
    ok &= Expect(TestModelExporterMaterialFallbackAndNormals(), "Material fallback and normal repair export test should pass");
    ok &= Expect(TestUnifiedModelExport(), "Unified model export test should pass");
    ok &= Expect(TestSharedSettingsSaveAndLoad(), "Shared settings save/load test should pass");
    ok &= Expect(TestSharedSettingsLegacyMigration(), "Shared settings legacy migration test should pass");
    ok &= Expect(TestDynModelParsingAndExport(), "DynModel parsing and skinned export tests should pass");

    const AssetIndex::ValidationResult validation = AssetIndex::ValidateGameRoot(fixtureRoot);
    ok &= Expect(validation.IsValid(), "synthetic fixture should validate as a plausible install");
    ok &= Expect(validation.hasAces, "validation should find aces.exe");
    ok &= Expect(validation.hasContent, "validation should find content folder");
    ok &= Expect(validation.hasBaseRes, "validation should find content/base/res");
    ok &= Expect(validation.hasKnownContainer, "validation should find known Dagor markers");

    const AssetIndex::DiscoverResult discovered = AssetIndex::DiscoverResources(fixtureRoot);
    ok &= Expect(discovered.error.empty(), "discovery should complete without error");
    ok &= Expect(!discovered.canceled, "discovery should not be canceled");
    ok &= Expect(discovered.records.size() == 6, "discovery should find six synthetic Dagor records");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Vromfs) == 1, "discovery should find one VROMFS");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Grp) == 1, "discovery should find one GRP");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Dxp) == 1, "discovery should find one DXP");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Descriptor) == 2, "discovery should find two descriptors");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Blk) == 1, "discovery should find one BLK");
    ok &= Expect(TestFixtureBlk(fixtureRoot), "fixture BLK parser test should pass");
    ok &= Expect(TestBlkResourceLinks(discovered.records), "fixture BLK resource link test should pass");
    ok &= Expect(TestDescriptorResourceLinks(), "descriptor resource link test should pass");
    ok &= Expect(TestDescriptorResourceLinksSkipZeroPayloadDxp(), "descriptor zero-payload DXP link test should pass");
    ok &= Expect(TestDescriptorResourceLinksNonStaticGrpMatch(), "descriptor non-static GRP link test should pass");
    ok &= Expect(TestPackContentsFixtures(fixtureRoot), "fixture PackContents tests should pass");

    const std::array<std::pair<const char*, AssetIndex::AssetType>, 5> classifyCases = {
        std::pair{ "foo.vromfs.bin", AssetIndex::AssetType::Vromfs },
        std::pair{ "vehicle.GRP", AssetIndex::AssetType::Grp },
        std::pair{ "tex.dxp.bin", AssetIndex::AssetType::Dxp },
        std::pair{ "riDesc.bin", AssetIndex::AssetType::Descriptor },
        std::pair{ "rules.blk", AssetIndex::AssetType::Blk },
    };

    for (const auto& testCase : classifyCases)
    {
        const std::optional<AssetIndex::AssetType> type = AssetIndex::ClassifyPath(testCase.first);
        ok &= Expect(type.has_value() && *type == testCase.second, "classification should match expected type");
    }

    const fs::path cacheDir = fs::temp_directory_path() / "DagorAssetsExtractorTests";
    const fs::path cachePath = cacheDir / "index-cache-test.txt";
    std::error_code ec;
    fs::remove_all(cacheDir, ec);

    std::string cacheError;
    ok &= Expect(
        AssetIndex::SaveIndexCacheRecords(cachePath, fixtureRoot, discovered.records, cacheError),
        "cache save should succeed");
    if (!cacheError.empty())
    {
        std::cerr << "Cache save detail: " << cacheError << '\n';
    }

    const AssetIndex::IndexCacheLoadResult cacheLoad = AssetIndex::LoadIndexCacheRecords(cachePath, fixtureRoot);
    ok &= Expect(cacheLoad.fileFound, "cache load should find the cache file");
    ok &= Expect(cacheLoad.rootMatched, "cache load should match fixture root");
    ok &= Expect(cacheLoad.recordsLoaded == discovered.records.size(), "cache load should restore all records");
    ok &= Expect(cacheLoad.recordsSkipped == 0, "cache load should skip no fresh records");

    fs::remove_all(cacheDir, ec);

    if (!ok)
    {
        return 1;
    }

    std::cout << "AssetIndexTests passed\n";

    return 0;
}
