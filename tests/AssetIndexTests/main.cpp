#include "AssetIndex/AssetIndex.h"
#include "DagorIO/BinaryReader.h"
#include "DagorFormats/VromfsReader.h"
#include "DagorFormats/DxpReader.h"
#include "DagorFormats/DdsxReader.h"
#include "DagorFormats/BlkReader.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
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
        //   0x11  uint8   depth = 1
        //   0x12  uint16  reserved = 0
        //   0x14  uint32  memorySize = 524288
        //   0x18  uint32  packedSize = 131072
        //   0x1C  uint32  compressionType = 2 (zstd)
        std::vector<uint8_t> data(32, 0);

        // Magic: "DDSx" -> 0x44 0x44 0x53 0x78
        data[0] = 0x44; data[1] = 0x44; data[2] = 0x53; data[3] = 0x78;

        // d3dFormat = 77 (BC3_UNORM)
        data[4] = 77; data[5] = 0; data[6] = 0; data[7] = 0;

        // flags = 0
        // (already zeroed)

        // width = 1024 (0x0400)
        data[12] = 0x00; data[13] = 0x04;

        // height = 512 (0x0200)
        data[14] = 0x00; data[15] = 0x02;

        // mipCount = 10
        data[16] = 10;

        // depth = 1
        data[17] = 1;

        // memorySize = 524288 (0x00080000)
        data[20] = 0x00; data[21] = 0x00; data[22] = 0x08; data[23] = 0x00;

        // packedSize = 131072 (0x00020000)
        data[24] = 0x00; data[25] = 0x00; data[26] = 0x02; data[27] = 0x00;

        // compressionType = 2 (zstd)
        data[28] = 2; data[29] = 0; data[30] = 0; data[31] = 0;

        DagorFormats::DdsxReader reader(data);
        reader.Parse();

        const DagorFormats::DdsxHeader& hdr = reader.GetHeader();
        ok &= Expect(hdr.IsValid(), "DdsxReader header should be valid");
        ok &= Expect(hdr.d3dFormat == 77, "DdsxReader d3dFormat should be 77 (BC3_UNORM)");
        ok &= Expect(hdr.width == 1024, "DdsxReader width should be 1024");
        ok &= Expect(hdr.height == 512, "DdsxReader height should be 512");
        ok &= Expect(hdr.mipCount == 10, "DdsxReader mipCount should be 10");
        ok &= Expect(hdr.depth == 1, "DdsxReader depth should be 1");
        ok &= Expect(hdr.IsCompressed(), "DdsxReader should detect compressed data");
        ok &= Expect(!hdr.IsOodleCompressed(), "DdsxReader should not detect Oodle");
        ok &= Expect(std::string(hdr.FormatName()) == "BC3_UNORM", "DdsxReader format name should be BC3_UNORM");
        ok &= Expect(std::string(hdr.CompressionName()) == "zstd", "DdsxReader compression name should be zstd");

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
}

int main(int argc, char** argv)
{
    fs::path repoRoot = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path fixtureRoot = repoRoot / "tests" / "fixtures" / "synthetic_wt";

    bool ok = true;

    ok &= Expect(TestBinaryReader(), "BinaryReader tests should pass");
    ok &= Expect(TestVromfsReader(), "VromfsReader tests should pass");
    ok &= Expect(TestDxpReader(), "DxpReader tests should pass");
    ok &= Expect(TestDdsxReader(), "DdsxReader tests should pass");
    ok &= Expect(TestBlkReader(), "BlkReader tests should pass");

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
