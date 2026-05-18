#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"
#include "AssetIndex/PackContents.h"
#include "DagorIO/BinaryReader.h"
#include "DagorFormats/VromfsReader.h"
#include "DagorFormats/DxpReader.h"
#include "DagorFormats/DdsxReader.h"
#include "DagorFormats/BlkReader.h"
#include "DagorFormats/GrpReader.h"
#include "DagorFormats/GameResDescReader.h"
#include "Jobs/JobSystem.h"
#include "Export/ExportManifest.h"
#include "Export/TextureExporter.h"
#include "Export/ModelExporter.h"
#include "DagorFormats/RendInstReader.h"

#include "DagorIO/Decompression.h"
#include <array>
#include <atomic>
#include <chrono>
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

        // Construct a synthetic 4+4 + 11 + 4+4+4+4 = 35 byte GRP
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
        ok &= Expect(files.size() == 1, "GrpReader should parse 1 file");
        if (!files.empty())
        {
            ok &= Expect(files[0].name == "test_model", "GrpReader name match");
            ok &= Expect(files[0].offset == 0x100, "GrpReader offset match");
            ok &= Expect(files[0].size == 10, "GrpReader size match");
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
        appendParam(0x02000000u | 9u, 1u);               // t2 texture slot references tex[1]

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
            ok &= Expect(entry.shaders.size() == 1 && entry.shaders[0] == "rendinst_simple",
                "GameResDescReader should collect shader class");
            ok &= Expect(entry.dependencyIds.size() == 1 && entry.dependencyIds[0] == 1,
                "GameResDescReader should collect material texture index references");
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
            }
        }

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
        cache.Clear();
        ok &= Expect(!cache.Has(dxpPath), "PackContentsCache should clear all entries");

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

    std::vector<uint8_t> MakeSyntheticDxpPack(const std::vector<std::string>& textureNames)
    {
        const size_t ddsHeaderStart = 0x100;
        const size_t block3Start = ddsHeaderStart + textureNames.size() * 0x20;
        const size_t payloadStart = block3Start + textureNames.size() * 0x18;
        std::vector<uint8_t> data(payloadStart + textureNames.size() * 4, 0);

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
        put16(0x8, static_cast<uint16_t>(textureNames.size()));
        put32(0x20, static_cast<uint32_t>(ddsHeaderStart - 0x10));
        put32(0x30, static_cast<uint32_t>(block3Start - 0x10));

        size_t nameOffset = 0x48;
        for (const std::string& textureName : textureNames)
        {
            std::copy(textureName.begin(), textureName.end(), data.begin() + nameOffset);
            nameOffset += textureName.size();
            data[nameOffset++] = 0;
        }

        for (size_t i = 0; i < textureNames.size(); ++i)
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
            put32(tableOffset + 0x10, 4);

            const size_t payloadOffset = payloadStart + i * 4;
            data[payloadOffset + 0] = 0x10;
            data[payloadOffset + 1] = 0x20;
            data[payloadOffset + 2] = 0x30;
            data[payloadOffset + 3] = 0x40;
        }

        return data;
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
        ok &= Expect(WriteBinaryFile(grpPath, MakeSyntheticGrpPack("sample_tank")),
            "Descriptor link test should write synthetic GRP");
        ok &= Expect(WriteBinaryFile(dxpPath, MakeSyntheticDxpPack({ "body_tex", "body_n" })),
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
            if (link.matchedGrpPath)
            {
                ok &= Expect(link.matchedGrpPath->filename() == "sample_vehicle.grp",
                    "Descriptor link should prefer BLK-linked GRP pack");
            }

            ok &= Expect(link.textureLinks.size() == 1,
                "Descriptor link should resolve material texture references, not every declared texture");
            if (!link.textureLinks.empty())
            {
                const AssetIndex::DescriptorTextureLink& textureLink = link.textureLinks[0];
                ok &= Expect(textureLink.textureIndex == 1, "Descriptor texture link should preserve texture slot index");
                ok &= Expect(textureLink.textureName == "body_n", "Descriptor texture link should resolve texture index to name");
                ok &= Expect(textureLink.matchedDxpPath.has_value(), "Descriptor texture link should match DXP pack");
                ok &= Expect(textureLink.matchedDxpEntryName.has_value() && *textureLink.matchedDxpEntryName == "body_n",
                    "Descriptor texture link should match DXP entry name");
                if (textureLink.matchedDxpPath)
                {
                    ok &= Expect(textureLink.matchedDxpPath->filename() == "sample_textures.dxp.bin",
                        "Descriptor texture link should prefer BLK-linked DXP pack");
                }
            }
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

        // Vertices
        pushBytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(DagorFormats::Vertex));
        // Indices
        pushBytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t));

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
    ok &= Expect(TestBlkReader(), "BlkReader tests should pass");
    ok &= Expect(TestGameResDescReader(), "GameResDescReader tests should pass");
    ok &= Expect(TestJobSystem(), "JobSystem tests should pass");
    ok &= Expect(TestDecompression(), "Decompression tests should pass");
    ok &= Expect(TestExportManifest(), "ExportManifest tests should pass");
    ok &= Expect(TestTextureExporterDX10(), "TextureExporter DX10 tests should pass");
    ok &= Expect(TestTextureExporterDxpPayloadExport(), "TextureExporter DXP payload export test should pass");
    ok &= Expect(TestTextureExporterMissingOodleFallback(), "TextureExporter missing-Oodle fallback test should pass");
    ok &= Expect(TestRendInstParsingAndExport(), "RendInst geometry parsing and GLB/OBJ export tests should pass");

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
